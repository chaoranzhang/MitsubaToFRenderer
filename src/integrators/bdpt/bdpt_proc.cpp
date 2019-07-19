/*
    This file is part of Mitsuba, a physically based rendering system.

    Copyright (c) 2007-2014 by Wenzel Jakob and others.

    Mitsuba is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Mitsuba is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <mitsuba/core/statistics.h>
#include <mitsuba/core/sfcurve.h>
#include <mitsuba/bidir/util.h>
#include "bdpt_proc.h"

MTS_NAMESPACE_BEGIN

/* ==================================================================== */
/*                         Worker implementation                        */
/* ==================================================================== */

class BDPTRenderer : public WorkProcessor {
public:
	BDPTRenderer(const BDPTConfiguration &config) : m_config(config) { }

	BDPTRenderer(Stream *stream, InstanceManager *manager)
		: WorkProcessor(stream, manager), m_config(stream) { }

	virtual ~BDPTRenderer() { }

	void serialize(Stream *stream, InstanceManager *manager) const {
		m_config.serialize(stream);
	}

	ref<WorkUnit> createWorkUnit() const {
		return new RectangularWorkUnit();
	}

	ref<WorkResult> createWorkResult() const {
		return new BDPTWorkResult(m_config, m_rfilter.get(),
			Vector2i(m_config.blockSize));
	}

	void prepare() {
		Scene *scene = static_cast<Scene *>(getResource("scene"));
		m_scene = new Scene(scene);
		m_sampler = static_cast<Sampler *>(getResource("sampler"));
		m_sensor = static_cast<Sensor *>(getResource("sensor"));
		m_rfilter = m_sensor->getFilm()->getReconstructionFilter();
		m_scene->removeSensor(scene->getSensor());
		m_scene->addSensor(m_sensor);
		m_scene->setSensor(m_sensor);
		m_scene->setSampler(m_sampler);
		m_scene->wakeup(NULL, m_resources);
		m_scene->initializeBidirectional();

		if((m_config.m_isldSampling || m_config.m_isAdaptive) && m_sampler->getSampleCount()%m_config.m_frames != 0)
			SLog(EError, "Number of samples (%i) must be integral multiple of number of frames (%i) "
					"if ldsampling or adaptive sampling is enabled", m_sampler->getSampleCount(), m_config.m_frames);

		m_ellipsoid = new Ellipsoid(scene->getMaxDepth(), scene->getPrimitiveCount());
	}

	void process(const WorkUnit *workUnit, WorkResult *workResult, const bool &stop) {
		const RectangularWorkUnit *rect = static_cast<const RectangularWorkUnit *>(workUnit);
		BDPTWorkResult *result = static_cast<BDPTWorkResult *>(workResult);
		bool needsTimeSample = m_sensor->needsTimeSample();
		Float time = m_sensor->getShutterOpen();

		result->setOffset(rect->getOffset());
		result->setSize(rect->getSize());
		result->clear();
		m_hilbertCurve.initialize(TVector2<uint8_t>(rect->getSize()));

		#if defined(MTS_DEBUG_FP)
			enableFPExceptions();
        #endif

        const ref_vector<Emitter> &emitters = m_scene->getEmitters();
		for(int i =0;i<emitters.size();i++){
			const Emitter *emitter = emitters[0].get();
			if (emitter->getType() & (Emitter::EOrthographicEmitter | Emitter::EPerspectiveEmitter)
				&& m_config.lightImage){
				Log(EError, "Set lightImage to be false in the integrator for projectors to work");
			}
		}

		Path emitterSubpath;
		Path sensorSubpath;

		/* Determine the necessary random walk depths based on properties of
		   the endpoints */
		int maxDepth = m_config.maxDepth;
		if(result->m_decompositionType == Film::ETransientEllipse)
			maxDepth--;

		int emitterDepth = maxDepth,
		    sensorDepth = maxDepth;

		/* Go one extra step if the sensor can be intersected */
		if (!m_scene->hasDegenerateSensor() && emitterDepth != -1)
			++emitterDepth;

		/* Go one extra step if there are emitters that can be intersected */
		if (!m_scene->hasDegenerateEmitters() && sensorDepth != -1)
			++sensorDepth;

		if(!m_config.m_isAdaptive){ //Not adaptive, so perform the regular technique
			for (size_t i=0; i<m_hilbertCurve.getPointCount(); ++i) {
				Point2i offset = Point2i(m_hilbertCurve[i]) + Vector2i(rect->getOffset());
				m_sampler->generate(offset);

				for (size_t j = 0; j<m_sampler->getSampleCount(); j++) {
					if (stop)
						break;

					if (needsTimeSample)
						time = m_sensor->sampleTime(m_sampler->next1D());

					/* Start new emitter and sensor subpaths */
					emitterSubpath.initialize(m_scene, time, EImportance, m_pool);
					sensorSubpath.initialize(m_scene, time, ERadiance, m_pool);

					/* Sample a random path length between pathMin and PathMax which will be equal to the total path for this path: TODO: Extend to multiple random path lengths? */
					Float pathLengthTarget;
					if(!m_config.m_isldSampling)
						pathLengthTarget = result->samplePathLengthTarget(m_sampler);
					else
						pathLengthTarget = m_config.m_decompositionMinBound + m_config.m_decompositionBinWidth*(j%m_config.m_frames) + m_config.m_decompositionBinWidth*m_sampler->nextFloat();

					// TODO: For transientEllipse, stop generating random paths after pathLength target
					/* Perform a random walk using alternating steps on each path */
					Path::alternatingRandomWalkFromPixel(m_scene, m_sampler,result,
						emitterSubpath, emitterDepth, sensorSubpath,
						sensorDepth, offset, m_config.rrDepth, m_pool);

					evaluate(result, emitterSubpath, sensorSubpath, pathLengthTarget);

					emitterSubpath.release(m_pool);
					sensorSubpath.release(m_pool);

					m_sampler->advance();
				}
			}
		}else {
			//Pre-process: mean of all the paths and all time bins
			const int nSamples = 1000;
			size_t totalPoints = m_hilbertCurve.getPointCount();

			BDPTConfiguration fakeConfig = m_config;
			fakeConfig.m_decompositionBinWidth = fakeConfig.m_decompositionMaxBound-fakeConfig.m_decompositionMinBound;
			fakeConfig.m_frames = 1; // mean value can be computed with average only

			// Create a fake work result to use the evaluate function and put either transient/transientEllipse case there
			BDPTWorkResult *fakeResult = new BDPTWorkResult(fakeConfig, m_rfilter.get(),
											Vector2i(m_config.blockSize));
			fakeResult->setOffset(rect->getOffset());
			fakeResult->setSize(rect->getSize());
			fakeResult->clear();

			Spectrum meanValue(0.0);

			/* Estimate the overall luminance on the image plane */
			for (int i=0; i<nSamples; ++i) {
				int index = floor(m_sampler->nextFloat()*totalPoints) ; // instead of m_sampler->nextSize(totalPoints) which seems slow
				Point2i offset = Point2i(m_hilbertCurve[index]) + Vector2i(rect->getOffset());
				m_sampler->generate(offset);
				Float pathLengthTarget = fakeResult->samplePathLengthTarget(m_sampler);


				// TODO: For transientEllipse, stop generating random paths after pathLength target
				/* Perform a random walk using alternating steps on each path */
				if (needsTimeSample)
					time = m_sensor->sampleTime(m_sampler->next1D());

				/* Start new emitter and sensor subpaths */
				emitterSubpath.initialize(m_scene, time, EImportance, m_pool);
								sensorSubpath.initialize(m_scene, time, ERadiance, m_pool);
				Path::alternatingRandomWalkFromPixel(m_scene, m_sampler,result,
					emitterSubpath, emitterDepth, sensorSubpath,
					sensorDepth, offset, m_config.rrDepth, m_pool);
				meanValue += evaluate(fakeResult, emitterSubpath, sensorSubpath, pathLengthTarget);

				emitterSubpath.release(m_pool);
				sensorSubpath.release(m_pool);
			}

			// Find average of fakeResult. Note: This is highly inefficient in rendering speed and needs a better way, probably by hacking evaluate to return the spectrum value?
			Spectrum averegeBitmap = fakeResult->average()/m_config.m_frames;
			meanValue = meanValue/nSamples;

			Float averageLuminance = averegeBitmap.getLuminance();

			Float sampleLuminance;

			// Adaptive rendering
			size_t samplesPerBin = m_sampler->getSampleCount()/m_config.m_frames;

			//
			int borderSize 	= result->getImageBlock()->getBorderSize();
			Float *target 	= (Float *) result->getImageBlock()->getBitmap()->getFloatData();
			Float *snapshot = (Float *) alloca(sizeof(Float)*
					3 * (2*borderSize+1)*(2*borderSize+1));
			int channels  	= result->getImageBlock()->getBitmap()->getChannelCount();


			for (size_t i=0; i<m_hilbertCurve.getPointCount(); ++i) {
				Point2i offset = Point2i(m_hilbertCurve[i]) + Vector2i(rect->getOffset());
				m_sampler->generate(offset);

				for (size_t j=0; j < m_config.m_frames; j++){

					/* Before starting to place samples within the area of a single pixel, the
					   following takes a snapshot of all surrounding channels+weight+alpha
					   values. Those are then used later to ensure that adjacent pixels will
					   not be disproportionately biased by this pixel's contributions. */
					for (int y=0, temp=0; y<2*borderSize+1; ++y) {
						for (int x=0; x<2*borderSize+1; ++x, ++temp) {
							Float *src = target + ( (m_hilbertCurve[i].y+y) * (size_t) rect->getSize().x + (m_hilbertCurve[i].x+x)) * channels + j;
							*(snapshot+temp) = *src;
						}
					}

					Float mean = 0, meanSqr = 0.0f;
					size_t sampleCount = 0;
					while (true) {
						if (stop)
							break;

						if (needsTimeSample)
							time = m_sensor->sampleTime(m_sampler->next1D());

						/* Start new emitter and sensor subpaths */
						emitterSubpath.initialize(m_scene, time, EImportance, m_pool);
						sensorSubpath.initialize(m_scene, time, ERadiance, m_pool);

						/* Sample a random path length between pathMin and PathMax which will be equal to the total path for this path: TODO: Extend to multiple random path lengths? */
						Float pathLengthTarget;
						if(!m_config.m_isldSampling)
							pathLengthTarget = result->samplePathLengthTarget(m_sampler);
						else
							pathLengthTarget = m_config.m_decompositionMinBound + m_config.m_decompositionBinWidth*j + m_config.m_decompositionBinWidth*m_sampler->nextFloat();

						// TODO: For transientEllipse, stop generating random paths after pathLength target
						/* Perform a random walk using alternating steps on each path */
						Path::alternatingRandomWalkFromPixel(m_scene, m_sampler,result,
							emitterSubpath, emitterDepth, sensorSubpath,
							sensorDepth, offset, m_config.rrDepth, m_pool);

						Spectrum sampleValue = evaluate(result, emitterSubpath, sensorSubpath, pathLengthTarget);

						emitterSubpath.release(m_pool);
						sensorSubpath.release(m_pool);

						m_sampler->advance();

						sampleLuminance = sampleValue.getLuminance();
						sampleCount++ ;
						const Float delta = sampleLuminance - mean;
						mean += delta / sampleCount;
						meanSqr += delta * (sampleLuminance - mean);

						if (m_config.m_adapMaxSampleFactor >= 0 && sampleCount >= m_config.m_adapMaxSampleFactor * samplesPerBin) {
							break;
						} else if (sampleCount >= samplesPerBin) {
							/* Variance of the primary estimator */
							const Float variance = meanSqr / (sampleCount-1);

							Float stdError = std::sqrt(variance/sampleCount);

							/* Half width of the confidence interval */
							Float ciWidth = stdError * m_config.m_adapQuantile;

							/* Relative error heuristic */
							Float base = std::max(mean, averageLuminance * 0.01f);

							if (ciWidth <= m_config.m_adapMaxError * base)
								break;
						}
					}
					/* Ensure that a large amounts of samples in one pixel do not
					   bias neighboring pixels (due to the reconstruction filter) */
					Float factor = (Float)samplesPerBin / (Float)sampleCount;
					for (int y=0, temp=0; y<2*borderSize+1; ++y) {
						for (int x=0; x<2*borderSize+1; ++x, ++temp) {
							Float *src = target + ( (m_hilbertCurve[i].y+y) * (size_t) rect->getSize().x + (m_hilbertCurve[i].x+x)) * channels + j;
							(*src) = *(snapshot+temp) * (1-factor) + (*src) * factor;
						}
					}
				}
			}
		}

		#if defined(MTS_DEBUG_FP)
			disableFPExceptions();
		#endif

		/* Make sure that there were no memory leaks */
		Assert(m_pool.unused());
	}

	/// Evaluate the contributions of the given eye and light paths
	Spectrum evaluate(BDPTWorkResult *wr,
			Path &emitterSubpath, Path &sensorSubpath, Float &pathLengthTarget) {
		/* Check if the emitter is laser?*/
		bool isEmitterLaser = false;
		const AbstractEmitter *AE = emitterSubpath.vertex(1)->getAbstractEmitter();
		if (!AE->needsPositionSample() && !AE->needsDirectionSample() ){
			isEmitterLaser = true;
		}

		//For adaptive renderer
		Spectrum meanSpectrum(0.0);

		Point2 initialSamplePos = sensorSubpath.vertex(1)->getSamplePosition();
		const Scene *scene = m_scene;
		PathVertex tempEndpoint, tempSample;
		PathEdge tempEdge, connectionEdge;
		int maxDepth = m_config.maxDepth;
		if(wr->m_decompositionType == Film::ETransientEllipse)
			maxDepth--;

		/* For transient rendering */
		PathEdge *connectionEdge1 = m_pool.allocEdge(),
				 *connectionEdge2 = m_pool.allocEdge();
		PathVertex *connectionVertex = m_pool.allocVertex();
		Float EllipticPathWeight; // only for the transientEllipse case

		// To combine BDPT and elliptic BDPT
		bool combine = wr->m_combineBDPTAndElliptic;
		Float corrWeight = 1.0f; // will hold the f(\|x\|) for the BDPT length also will be equal to BDPT_pdf if BDPT is selected and Elliptic_pdf if Elliptic-BDPT is selected

		/* Compute the combined path lengths of the two subpaths */
		Float *emitterPathlength = NULL;
		Float *sensorPathlength = NULL;



		if (wr->m_decompositionType != Film::ESteadyState) {
			emitterPathlength = (Float *) alloca(emitterSubpath.vertexCount() * sizeof(Float));
			sensorPathlength = (Float *) alloca(sensorSubpath.vertexCount() * sizeof(Float));

			emitterPathlength[0] = sensorPathlength[0] = Float(0.0f);
			emitterPathlength[1] = sensorPathlength[1] = Float(0.0f);
			if (wr->m_decompositionType == Film::ETransient || wr->m_decompositionType == Film::ETransientEllipse) {
				for (size_t i = 2; i < emitterSubpath.vertexCount(); ++i){
					emitterPathlength[i] = emitterPathlength[i-1] + emitterSubpath.edge(i - 1)->length;
				}

				for (size_t i = 2; i < sensorSubpath.vertexCount(); ++i){
					sensorPathlength[i] = sensorPathlength[i-1] + sensorSubpath.edge(i - 1)->length;
				}
			} else if (wr->m_decompositionType == Film::EBounce) {
				for (size_t i = 2; i < emitterSubpath.vertexCount(); ++i){
					emitterPathlength[i] = emitterPathlength[i-1] + 1.0f;
				}

				for (size_t i = 2; i < sensorSubpath.vertexCount(); ++i){
					sensorPathlength[i] = sensorPathlength[i-1] + 1.0f;
				}
			}
		}

		/* Compute the combined weights along the two subpaths */
		Spectrum *importanceWeights = (Spectrum *) alloca(emitterSubpath.vertexCount() * sizeof(Spectrum)),
				 *radianceWeights  = (Spectrum *) alloca(sensorSubpath.vertexCount()  * sizeof(Spectrum));

		importanceWeights[0] = radianceWeights[0] = Spectrum(1.0f);
		for (size_t i=1; i<emitterSubpath.vertexCount(); ++i)
			importanceWeights[i] = importanceWeights[i-1] *
				emitterSubpath.vertex(i-1)->weight[EImportance] *
				emitterSubpath.vertex(i-1)->rrWeight *
				emitterSubpath.edge(i-1)->weight[EImportance];

		for (size_t i=1; i<sensorSubpath.vertexCount(); ++i)
			radianceWeights[i] = radianceWeights[i-1] *
				sensorSubpath.vertex(i-1)->weight[ERadiance] *
				sensorSubpath.vertex(i-1)->rrWeight *
				sensorSubpath.edge(i-1)->weight[ERadiance];

		Spectrum sampleValue(0.0f);


		Float *sampleDecompositionValue = NULL;
		Float *l_sampleDecompositionValue = NULL;
		Float *temp = NULL;

		if (wr->m_decompositionType != Film::ESteadyState) {
			sampleDecompositionValue 	= (Float *) alloca(sizeof(Float) * wr->getChannelCount());
			l_sampleDecompositionValue 	= (Float *) alloca(sizeof(Float) * wr->getChannelCount());
			temp = (Float *) alloca(sizeof(Float) * SPECTRUM_SAMPLES); // Assuming that SPECTRUM_SAMPLES = 3;

			for (int i=0; i<wr->getChannelCount(); ++i){
				sampleDecompositionValue[i]=0.0f;
				l_sampleDecompositionValue[i]=0.0f;
			}
			l_sampleDecompositionValue[wr->getChannelCount() - 1]=1.0f;
			l_sampleDecompositionValue[wr->getChannelCount() - 2]=1.0f;
		}

		for (int s = (int) emitterSubpath.vertexCount()-1; s >= 0; --s) {
			/* Determine the range of sensor vertices to be traversed,
			   while respecting the specified maximum path length */
			int minT = std::max(2-s, m_config.lightImage ? 0 : 2),
			    maxT = (int) sensorSubpath.vertexCount() - 1;
			if (m_config.maxDepth != -1)
				maxT = std::min(maxT, maxDepth + 1 - s);

			for (int t = maxT; t >= minT; --t) {
				if(s == 0 || t == 0 || (wr->m_decompositionType == Film::ETransient && s==1 && t==1)){
					continue; // hack to remove paths that are not taken care in transientEllipse
				}
				if(isEmitterLaser && wr->m_decompositionType == Film::ETransient && s==2 && t==1){ //First bounce of transient is not rendered if we have laser emitter
					continue;
				}
				if(wr->m_forceBounces && (s != wr->m_sBounces || t != wr->m_tBounces)){
					continue;
				}
				PathVertex
					*vsPred = emitterSubpath.vertexOrNull(s-1),
					*vtPred = sensorSubpath.vertexOrNull(t-1),
					*vs = emitterSubpath.vertex(s),
					*vt = sensorSubpath.vertex(t);
				PathEdge
					*vsEdge = emitterSubpath.edgeOrNull(s-1),
					*vtEdge = sensorSubpath.edgeOrNull(t-1);

				Film::EDecompositionType currentDecompositionType = wr->m_decompositionType;

				RestoreMeasureHelper rmh0(vs), rmh1(vt);

				/* Will be set to true if direct sampling was used */
				bool sampleDirect = false;

				/* Stores the pixel position associated with this sample */
				Point2 samplePos = initialSamplePos;

				/* Allowed remaining number of ENull vertices that can
				   be bridged via pathConnect (negative=arbitrarily many) */
				int remaining = m_config.maxDepth - s - t + 1;

				/* Will receive the path weight of the (s, t)-connection */
				Spectrum value;

				/* Total path length of this particular (s, t)-connection */
				Float pathLength = 0.0f;
				Float tempPathLength = 0.0f;

				/* Account for the terms of the measurement contribution
				   function that are coupled to the connection endpoints */
				if (vs->isEmitterSupernode()) {
					continue; //FIXME: Hack to avoid supernodes
					/* If possible, convert 'vt' into an emitter sample */
					if (!vt->cast(scene, PathVertex::EEmitterSample) || vt->isDegenerate())
						continue;

					value = radianceWeights[t] *
						vs->eval(scene, vsPred, vt, EImportance) *
						vt->eval(scene, vtPred, vs, ERadiance);

					if (currentDecompositionType != Film::ESteadyState) {
						pathLength = sensorPathlength[t]; // FIXME: Need to add newly casted vt distance?
						if( combine && (currentDecompositionType == Film::ETransientEllipse) && (pathLength >= wr->m_decompositionMinBound) && (pathLength <= wr->m_decompositionMaxBound)){
							currentDecompositionType = Film::ETransient;
						}
					}
				} else if (vt->isSensorSupernode()) {
					continue; //FIXME: Hack to avoid supernodes
					/* If possible, convert 'vs' into an sensor sample */
					if (!vs->cast(scene, PathVertex::ESensorSample) || vs->isDegenerate())
						continue;

					/* Make note of the changed pixel sample position */
					if (!vs->getSamplePosition(vsPred, samplePos))
						continue;

					value = importanceWeights[s] *
						vs->eval(scene, vsPred, vt, EImportance) *
						vt->eval(scene, vtPred, vs, ERadiance);

					if (currentDecompositionType != Film::ESteadyState) {
						pathLength = emitterPathlength[s]; // FIXME: Need to add newly casted vt distance?
						if( combine && (currentDecompositionType == Film::ETransientEllipse) && (pathLength >= wr->m_decompositionMinBound) && (pathLength <= wr->m_decompositionMaxBound)){
							currentDecompositionType = Film::ETransient;
						}
					}

				} else if (m_config.sampleDirect && ((t == 1 && s > 1) || (s == 1 && t > 1))) {
					/* s==1/t==1 path: use a direct sampling strategy if requested */
					if (s == 1) {
						if (vt->isDegenerate())
							continue;
						/* Generate a position on an emitter using direct sampling */
						value = radianceWeights[t] * vt->sampleDirect(scene, m_sampler,
							&tempEndpoint, &tempEdge, &tempSample, EImportance);

						if (currentDecompositionType != Film::ESteadyState) {
							pathLength = sensorPathlength[t];
						}

						if (value.isZero())
							continue;
						vs = &tempSample; vsPred = &tempEndpoint; vsEdge = &tempEdge;
						value *= vt->eval(scene, vtPred, vs, ERadiance);


						/* FIXME */
						Spectrum throughputS(1.0f); // Understand the functioning of throughputS. May be it is not needed in this case, and also the rrDepth may not be needed in this case

						if(currentDecompositionType == Film::ETransient || currentDecompositionType == Film::ETransientEllipse){
							tempPathLength = pathLength + distance(vs->getPosition(),vt->getPosition());
						}

						if( combine && (currentDecompositionType == Film::ETransientEllipse) && (tempPathLength >= wr->m_decompositionMinBound) && (tempPathLength <= wr->m_decompositionMaxBound)){
							currentDecompositionType = Film::ETransient;
						}

						if(currentDecompositionType == Film::ETransientEllipse){
							if(!combine || tempPathLength <= wr->m_decompositionMinBound){// Adding additional vertex can only increase path length
								Float PathLengthRemaining = pathLengthTarget - emitterPathlength[s] - sensorPathlength[t];
								if(PathLengthRemaining < 0 || !(vs->EllipsoidalSampleBetween(scene, m_sampler, vs, vsEdge,
																											   vt, vtEdge,
																											   connectionVertex, connectionEdge1, connectionEdge2, PathLengthRemaining,
																											   EllipticPathWeight,
																											   EImportance,(int) emitterSubpath.vertexCount() > m_config.rrDepth, &throughputS)))
									continue;
							}
							else
								continue;
						}

						if (currentDecompositionType == Film::ETransientEllipse) {
							pathLength += connectionEdge1->length+connectionEdge2->length;
						}else if (currentDecompositionType == Film::ETransient) {
						//	pathLength += distance(vs->getPosition(),vt->getPosition());
							pathLength = tempPathLength;
						} else if (currentDecompositionType == Film::EBounce) {
							pathLength += 1.0f;
						}

						vt->measure = EArea;
					} else {
						if (vs->isDegenerate())
							continue;
						/* Generate a position on the sensor using direct sampling */
						value = importanceWeights[s] * vs->sampleDirect(scene, m_sampler,
							&tempEndpoint, &tempEdge, &tempSample, ERadiance);

						if (currentDecompositionType != Film::ESteadyState) {
							pathLength = emitterPathlength[s];
						}

						if (value.isZero())
							continue;
						vt = &tempSample; vtPred = &tempEndpoint; vtEdge = &tempEdge;
						value *= vs->eval(scene, vsPred, vt, EImportance);

						/* FIXME */
						Spectrum throughputS(1.0f); // Understand the functioning of throughputS. May be it is not needed in this case, and also the rrDepth may not be needed in this case

						if(currentDecompositionType == Film::ETransient || currentDecompositionType == Film::ETransientEllipse){
							tempPathLength = pathLength + distance(vs->getPosition(),vt->getPosition());
						}

						if( combine && (currentDecompositionType == Film::ETransientEllipse) && (tempPathLength >= wr->m_decompositionMinBound) && (tempPathLength <= wr->m_decompositionMaxBound)){
							// Decide whether to do BDPT or elliptic.
							if(wr->getModulationType() != PathLengthSampler::ENone){
								corrWeight = wr->correlationFunction(tempPathLength);
								if(m_sampler->nextFloat() < corrWeight)
									currentDecompositionType = Film::ETransient;
								else
									corrWeight = 1 - corrWeight;
								corrWeight = 1/corrWeight; // To increase speed.
					 		}else{
								currentDecompositionType = Film::ETransient;
					 		}
						}

						if(currentDecompositionType == Film::ETransientEllipse){
							SLog(EError, "Cannot make Direct Ellipsoidal connections");
							if(!combine || tempPathLength <= wr->m_decompositionMinBound){ // Adding additional vertex can only increase path length
								Float PathLengthRemaining = pathLengthTarget - emitterPathlength[s] - sensorPathlength[t];
								if(PathLengthRemaining < 0 || !(vs->EllipsoidalSampleBetween(scene, m_sampler, vs, vsEdge,
																											   vt, vtEdge,
																											   connectionVertex, connectionEdge1, connectionEdge2, PathLengthRemaining,
																											   EllipticPathWeight,
																											   EImportance,(int) emitterSubpath.vertexCount() > m_config.rrDepth, &throughputS)))
									continue;
							}else
								continue;
						}


						if (currentDecompositionType == Film::ETransientEllipse) {
							pathLength += connectionEdge1->length+connectionEdge2->length;
						}else if (currentDecompositionType == Film::ETransient) {
							//	pathLength += distance(vs->getPosition(),vt->getPosition());
								pathLength = tempPathLength;
						} else if (currentDecompositionType == Film::EBounce) {
							pathLength += 1.0f;
						}

						vs->measure = EArea;
					}

					sampleDirect = true;
				} else {

					/* Can't connect degenerate endpoints */
					if (vs->isDegenerate() || vt->isDegenerate())
						continue;


					if(currentDecompositionType == Film::ETransient || currentDecompositionType == Film::ETransientEllipse){
						tempPathLength = emitterPathlength[s]+sensorPathlength[t]+distance(vs->getPosition(),vt->getPosition());
					}

					if( combine && (currentDecompositionType == Film::ETransientEllipse) && (tempPathLength >= wr->m_decompositionMinBound) && (tempPathLength <= wr->m_decompositionMaxBound)){
						// Decide whether to do BDPT or elliptic.
						if(wr->getModulationType() != PathLengthSampler::ENone){
							corrWeight = wr->correlationFunction(tempPathLength);
							if(m_sampler->nextFloat() < corrWeight)
								currentDecompositionType = Film::ETransient;
							else
								corrWeight = 1 - corrWeight;
							corrWeight = 1/corrWeight; // To increase speed.
				 		}else{
							currentDecompositionType = Film::ETransient;
				 		}
					}

					if(currentDecompositionType != Film::ETransientEllipse)
						value = importanceWeights[s] * radianceWeights[t] *
							vs->eval(scene, vsPred, vt, EImportance) *
							vt->eval(scene, vtPred, vs, ERadiance);
					else
						value = importanceWeights[s] * radianceWeights[t];

					/* FIXME */
					Spectrum throughputS(1.0f); // Understand the functioning of throughputS. May be it is not needed in this case, and also the rrDepth may not be needed in this case

					if(currentDecompositionType == Film::ETransientEllipse){
						if(!combine || tempPathLength <= wr->m_decompositionMinBound){ // Adding additional vertex can only increase path length
							Float PathLengthRemaining = pathLengthTarget - emitterPathlength[s] - sensorPathlength[t];

							if(!value.isZero() && PathLengthRemaining > 0){
//							if(!value.isZero()){
								EMeasure vsMeasure = vs->measure;
								EMeasure vtMeasure = vt->measure;

								vs->measure = vt->measure = EArea;

//								Float miWeight = 1.0/(s+t-1-isEmitterLaser);
//								Path::miWeight(scene, emitterSubpath, &connectionEdge,
//									sensorSubpath, s, t, m_config.sampleDirect, m_config.lightImage);

								vs->measure = vsMeasure;
								vt->measure = vtMeasure;

								 tempPathLength = emitterPathlength[s] + sensorPathlength[t];
								 vs->EllipsoidalSampleBetween(scene, m_sampler, vsPred, vs, vsEdge,
																			   vtPred, vt, vtEdge,
																			   emitterSubpath, sensorSubpath, s, t, isEmitterLaser,
																			   connectionVertex, connectionEdge1, connectionEdge2, PathLengthRemaining, tempPathLength,
																			   EllipticPathWeight, corrWeight, value, sampleValue, meanSpectrum,
																			   sampleDecompositionValue, l_sampleDecompositionValue, temp, samplePos, m_ellipsoid,
																			   EImportance, wr);
							}
							continue;
						}else
							continue;
					}

					if (currentDecompositionType == Film::ETransientEllipse) {
						pathLength = emitterPathlength[s]+sensorPathlength[t]+connectionEdge1->length+connectionEdge2->length;
					}else if (currentDecompositionType == Film::ETransient) {
//						pathLength = emitterPathlength[s]+sensorPathlength[t]+distance(vs->getPosition(),vt->getPosition());
						pathLength = tempPathLength;
					}else if (currentDecompositionType == Film::EBounce) {
						pathLength = emitterPathlength[s]+sensorPathlength[t]+1.0f;
					}

					/* Temporarily force vertex measure to EArea. Needed to
					   handle BSDFs with diffuse + specular components */
					vs->measure = vt->measure = EArea;

				}

				/* Attempt to connect the two endpoints, which could result in
				   the creation of additional vertices (index-matched boundaries etc.) */
				int interactions = remaining; // backup

				if(currentDecompositionType != Film::ETransientEllipse){

					if (value.isZero() || !connectionEdge.pathConnectAndCollapse(
							scene, vsEdge, vs, vt, vtEdge, interactions))
						continue;



					/* Account for the terms of the measurement contribution
					   function that are coupled to the connection edge */
					if (!sampleDirect)
						value *= connectionEdge.evalCached(vs, vt, PathEdge::EGeneralizedGeometricTerm);
					else
						value *= connectionEdge.evalCached(vs, vt, PathEdge::ETransmittance |
								(s == 1 ? PathEdge::ECosineRad : PathEdge::ECosineImp));


					if (sampleDirect) {
						/* A direct sampling strategy was used, which generated
						   two new vertices at one of the path ends. Temporarily
						   modify the path to reflect this change */
						if (t == 1)
							sensorSubpath.swapEndpoints(vtPred, vtEdge, vt);
						else
							emitterSubpath.swapEndpoints(vsPred, vsEdge, vs);
					}

					/* Compute the multiple importance sampling weight */
					Float miWeight = Path::miWeight(scene, emitterSubpath, &connectionEdge,
						sensorSubpath, s, t, m_config.sampleDirect, m_config.lightImage);
//					Float miWeight = 1.0/(s+t-1-isEmitterLaser);

					if (sampleDirect) {
						/* Now undo the previous change */
						if (t == 1)
							sensorSubpath.swapEndpoints(vtPred, vtEdge, vt);
						else
							emitterSubpath.swapEndpoints(vsPred, vsEdge, vs);
					}

					/* Determine the pixel sample position when necessary */
					if (vt->isSensorSample() && !vt->getSamplePosition(vs, samplePos))
						continue;

					#if BDPT_DEBUG == 1
						/* When the debug mode is on, collect samples
						   separately for each sampling strategy. Note: the
						   following piece of code artificially increases the
						   exposure of longer paths */
						Spectrum splatValue = value * (m_config.showWeighted
							? miWeight : 1.0f);// * std::pow(2.0f, s+t-3.0f));
						wr->putDebugSample(s, t, samplePos, splatValue);
					#endif


					if(currentDecompositionType != Film::ESteadyState){
						if(currentDecompositionType == Film::ETransient && wr->getModulationType() != PathLengthSampler::ENone)
								miWeight *= wr->correlationFunction(pathLength)*corrWeight;
						else{
							size_t binIndex = floor((pathLength - wr->m_decompositionMinBound)/(wr->m_decompositionBinWidth));
							if ( pathLength >= wr->m_decompositionMinBound && pathLength <= wr->m_decompositionMaxBound && !value.isZero() && currentDecompositionType != Film::ESteadyState && binIndex >= 0 && binIndex < wr->m_frames){
								if(SPECTRUM_SAMPLES == 3)
									value.toLinearRGB(temp[0],temp[1],temp[2]); // Verify what happens when SPECTRUM_SAMPLES ! = 3
								else
									SLog(EError, "cannot run transient renderer for spectrum values more than 3");

								if (t>=2){
									sampleDecompositionValue[binIndex*SPECTRUM_SAMPLES+0] += temp[0] * miWeight;
									sampleDecompositionValue[binIndex*SPECTRUM_SAMPLES+1] += temp[1] * miWeight;
									sampleDecompositionValue[binIndex*SPECTRUM_SAMPLES+2] += temp[2] * miWeight;
								}else if(t==1){
									// FIXME: This is very inefficient. l_sampleDecompositionValue is very sparse. In fact, we only write to bin with binIndex
									l_sampleDecompositionValue[binIndex*SPECTRUM_SAMPLES+0] += temp[0] * miWeight;
									l_sampleDecompositionValue[binIndex*SPECTRUM_SAMPLES+1] += temp[1] * miWeight;
									l_sampleDecompositionValue[binIndex*SPECTRUM_SAMPLES+2] += temp[2] * miWeight;
									wr->putLightSample(samplePos, l_sampleDecompositionValue);
									//reset the l_sampleDecompositionValue
									l_sampleDecompositionValue[binIndex*SPECTRUM_SAMPLES+0] = 0;
									l_sampleDecompositionValue[binIndex*SPECTRUM_SAMPLES+1] = 0;
									l_sampleDecompositionValue[binIndex*SPECTRUM_SAMPLES+2] = 0;
								}
							}
						}
					}

					if ( currentDecompositionType == Film::ESteadyState  || (wr->m_decompositionType == Film::ETransient && wr->getModulationType() != PathLengthSampler::ENone)){
						if (t >= 2)
							sampleValue += value * miWeight;
						else
							wr->putLightSample(samplePos, value * miWeight);
					}
				}
			}
		}
		if (wr->m_decompositionType == Film::ESteadyState || ( (wr->m_decompositionType == Film::ETransient || wr->m_decompositionType == Film::ETransientEllipse) && wr->getModulationType() != PathLengthSampler::ENone)) {
			wr->putSample(initialSamplePos, sampleValue);
		} else {
			sampleDecompositionValue[wr->getChannelCount()-2]=1.0f;
			sampleDecompositionValue[wr->getChannelCount()-1]=1.0f;
			wr->putSample(initialSamplePos, sampleDecompositionValue);
		}

		m_pool.release(connectionEdge1);
		m_pool.release(connectionEdge2);
		m_pool.release(connectionVertex);
		return meanSpectrum;
	}

	ref<WorkProcessor> clone() const {
		return new BDPTRenderer(m_config);
	}

	MTS_DECLARE_CLASS()
private:
	ref<Scene> m_scene;
	ref<Sensor> m_sensor;
	ref<Sampler> m_sampler;
	ref<ReconstructionFilter> m_rfilter;
	MemoryPool m_pool;
	BDPTConfiguration m_config;
	HilbertCurve2D<uint8_t> m_hilbertCurve;

	Ellipsoid *m_ellipsoid;
};


/* ==================================================================== */
/*                           Parallel process                           */
/* ==================================================================== */

BDPTProcess::BDPTProcess(const RenderJob *parent, RenderQueue *queue,
		const BDPTConfiguration &config) :
	BlockedRenderProcess(parent, queue, config.blockSize), m_config(config) {
	m_refreshTimer = new Timer();
}

ref<WorkProcessor> BDPTProcess::createWorkProcessor() const {
	return new BDPTRenderer(m_config);
}

void BDPTProcess::develop() {
	if (!m_config.lightImage)
		return;
	LockGuard lock(m_resultMutex);
	const ImageBlock *lightImage = m_result->getLightImage();
	m_film->setBitmap(m_result->getImageBlock()->getBitmap());

	m_film->addBitmap(lightImage->getBitmap(), 1.0f / m_config.sampleCount);

	m_refreshTimer->reset();
	m_queue->signalRefresh(m_parent);
}

void BDPTProcess::processResult(const WorkResult *wr, bool cancelled) {
	if (cancelled)
		return;
	const BDPTWorkResult *result = static_cast<const BDPTWorkResult *>(wr);
	ImageBlock *block = const_cast<ImageBlock *>(result->getImageBlock());
	LockGuard lock(m_resultMutex);
	m_progress->update(++m_resultCount);
	if (m_config.lightImage) {
		const ImageBlock *lightImage = m_result->getLightImage();
		m_result->put(result);
		if (m_parent->isInteractive()) {
			/* Modify the finished image block so that it includes the light image contributions,
			   which creates a more intuitive preview of the rendering process. This is
			   not 100% correct but doesn't matter, as the shown image will be properly re-developed
			   every 2 seconds and once more when the rendering process finishes */

			Float invSampleCount = 1.0f / m_config.sampleCount;
			const Bitmap *sourceBitmap = lightImage->getBitmap();
			Bitmap *destBitmap = block->getBitmap();
			int borderSize = block->getBorderSize();
			Point2i offset = block->getOffset();
			Vector2i size = block->getSize();

			for (int y=0; y<size.y; ++y) {
				const Float *source = sourceBitmap->getFloatData()
					+ (offset.x + (y+offset.y) * sourceBitmap->getWidth()) * SPECTRUM_SAMPLES;
				Float *dest = destBitmap->getFloatData()
					+ (borderSize + (y + borderSize) * destBitmap->getWidth()) * (SPECTRUM_SAMPLES + 2);

				for (int x=0; x<size.x; ++x) {
					Float weight = dest[SPECTRUM_SAMPLES + 1] * invSampleCount;
					for (int k=0; k<SPECTRUM_SAMPLES; ++k)
						*dest++ += *source++ * weight;
					dest += 2;
				}
			}
		}
	}

	m_film->put(block);

	/* Re-develop the entire image every two seconds if partial results are
	   visible (e.g. in a graphical user interface). This only applies when
	   there is a light image. */
	bool developFilm = m_config.lightImage &&
		(m_parent->isInteractive() && m_refreshTimer->getMilliseconds() > 2000);

	m_queue->signalWorkEnd(m_parent, result->getImageBlock(), false);

	if (developFilm)
		develop();
}

void BDPTProcess::bindResource(const std::string &name, int id) {
	BlockedRenderProcess::bindResource(name, id);
	if (name == "sensor" && m_config.lightImage) {
		/* If needed, allocate memory for the light image */
		m_result = new BDPTWorkResult(m_config, NULL, m_film->getCropSize());
		m_result->clear();
	}
}

MTS_IMPLEMENT_CLASS_S(BDPTRenderer, false, WorkProcessor)
MTS_IMPLEMENT_CLASS(BDPTProcess, false, BlockedRenderProcess)
MTS_NAMESPACE_END
