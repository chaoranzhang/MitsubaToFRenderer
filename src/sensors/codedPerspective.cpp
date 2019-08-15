#include <mitsuba/render/sensor.h>
#include <mitsuba/render/medium.h>
#include <mitsuba/core/track.h>
#include <mitsuba/core/frame.h>

MTS_NAMESPACE_BEGIN

/*!\plugin{codedPerspective}{Coded Perspective pinhole camera}
 * \order{1}
 * \parameters{
 *     \parameter{toWorld}{\Transform\Or\Animation}{
 *	      Specifies an optional camera-to-world transformation.
 *        \default{none (i.e. camera space $=$ world space)}
 *     }
 *     \parameter{focalLength}{\String}{
 *         Denotes the camera's focal length specified using
 *         \code{35mm} film equivalent units. See the main
 *         description for further details.
 *         \default{\code{50mm}}
 *     }
 *     \parameter{fov}{\Float}{
 *         An alternative to \code{focalLength}:
 *         denotes the camera's field of view in degrees---must be
 *         between 0 and 180, excluding the extremes.
 *     }
 *     \parameter{fovAxis}{\String}{
 *         When the parameter \code{fov} is given (and only then),
 *         this parameter further specifies the image axis, to
 *         which it applies.
 *         \begin{enumerate}[(i)]
 *             \item \code{\textbf{x}}: \code{fov} maps to the
 *                 \code{x}-axis in screen space.
 *             \item \code{\textbf{y}}: \code{fov} maps to the
 *                 \code{y}-axis in screen space.
 *             \item \code{\textbf{diagonal}}: \code{fov}
 *                maps to the screen diagonal.
 *             \item \code{\textbf{smaller}}: \code{fov}
 *                maps to the smaller dimension
 *                (e.g. \code{x} when \code{width}<\code{height})
 *             \item \code{\textbf{larger}}: \code{fov}
 *                maps to the larger dimension
 *                (e.g. \code{y} when \code{width}<\code{height})
 *         \end{enumerate}
 *         The default is \code{\textbf{x}}.
 *     }
 *     \parameter{shutterOpen, shutterClose}{\Float}{
 *         Specifies the time interval of the measurement---this
 *         is only relevant when the scene is in motion.
 *         \default{0}
 *     }
 *     \parameter{nearClip, farClip}{\Float}{
 *         Distance to the near/far clip
 *         planes.\default{\code{near\code}-\code{Clip=1e-2} (i.e.
 *         \code{0.01}) and {\code{farClip=1e4} (i.e. \code{10000})}}
 *     }
 *     \parameter{filename}{\String}{
 *       Filename of the coded camera mask image to be loaded;
 *       must be in latitude-longitude format.
 *     }
 * }
 *
 * This plugin implements a simple idealizied coded perspective camera model, which
 * has an infinitely small aperture. This creates an infinite depth of field,
 * i.e. no optical blurring occurs. The camera is can be specified to move during
 * an exposure, hence temporal blur is still possible.
 *
 * By default, the camera's field of view is specified using a 35mm film
 * equivalent focal length, which is first converted into a diagonal field
 * of view and subsequently applied to the camera. This assumes that
 * the film's aspect ratio matches that of 35mm film (1.5:1), though the
 * parameter still behaves intuitively when this is not the case.
 * Alternatively, it is also possible to specify a field of view in degrees
 * along a given axis (see the \code{fov} and \code{fovAxis} parameters).
 *
 * As for the camera's mask, the camera scales the input image to fit its film size
 * and consider the scaled image as a mask over the aperture.
 *
 * The exact camera position and orientation is most easily expressed using the
 * \code{lookat} tag, i.e.:
 * \begin{xml}
 * <sensor type="codedPerspective">
 *     <transform name="toWorld">
 *         <!-- Move and rotate the camera so that looks from (1, 1, 1) to (1, 2, 1)
 *              and the direction (0, 0, 1) points "up" in the output image -->
 *         <lookat origin="1, 1, 1" target="1, 2, 1" up="0, 0, 1"/>
 *     </transform>
 *     </string name="filename" value="image.png" />
 * </sensor>
 * \end{xml}
 */

class CodedPerspectiveCameraImpl : public CodedPerspectiveCamera {
public:
    CodedPerspectiveCameraImpl(const Properties &props)
            : CodedPerspectiveCamera(props) {
        /* This sensor is the result of a limiting process where the aperture
           radius tends to zero. However, it still has all the cosine
           foreshortening terms caused by the aperture, hence the flag "EOnSurface" */
        m_type |= EDeltaPosition | EPerspectiveCamera | EOnSurface | EDirectionSampleMapsToPixels;

        if (props.getAnimatedTransform("toWorld", Transform())->eval(0).hasScale())
            Log(EError, "Scale factors in the camera-to-world "
                        "transformation are not allowed!");
    }

    CodedPerspectiveCameraImpl(Stream *stream, InstanceManager *manager)
            : CodedPerspectiveCamera(stream, manager) {
        configure();
    }

    void configure() {
        CodedPerspectiveCamera::configure();

        const Vector2i &filmSize   = m_film->getSize();
        const Vector2i &cropSize   = m_film->getCropSize();
        const Point2i  &cropOffset = m_film->getCropOffset();

        Vector2 relSize((Float) cropSize.x / (Float) filmSize.x,
                        (Float) cropSize.y / (Float) filmSize.y);
        Point2 relOffset((Float) cropOffset.x / (Float) filmSize.x,
                         (Float) cropOffset.y / (Float) filmSize.y);

        /**
         * These do the following (in reverse order):
         *
         * 1. Create transform from camera space to [-1,1]x[-1,1]x[0,1] clip
         *    coordinates (not taking account of the aspect ratio yet)
         *
         * 2+3. Translate and scale to shift the clip coordinates into the
         *    range from zero to one, and take the aspect ratio into account.
         *
         * 4+5. Translate and scale the coordinates once more to account
         *     for a cropping window (if there is any)
         */

        std::cout<<"camera";
        m_cameraToSample =
                Transform::scale(Vector(1.0f / relSize.x, 1.0f / relSize.y, 1.0f))
                * Transform::translate(Vector(-relOffset.x, -relOffset.y, 0.0f))
                * Transform::scale(Vector(-0.5f, -0.5f*m_aspect, 1.0f))
                * Transform::translate(Vector(-1.0f, -1.0f/m_aspect, 0.0f))
                * Transform::perspective(m_xfov, m_nearClip, m_farClip);
        std::cout<<m_xfov<<" "<<m_nearClip<<" "<<m_farClip<<"\n";
        std::cout<<m_cameraToSample.toString()<<"\n";
        std::cout<<m_aspect<<" "<<m_resolution.x<<" "<<m_invResolution.x<<"\n";

        m_sampleToCamera = m_cameraToSample.inverse();

        /* Position differentials on the near plane */
        m_dx = m_sampleToCamera(Point(m_invResolution.x, 0.0f, 0.0f))
               - m_sampleToCamera(Point(0.0f));
        m_dy = m_sampleToCamera(Point(0.0f, m_invResolution.y, 0.0f))
               - m_sampleToCamera(Point(0.0f));

        /* Precompute some data for importance(). Please
           look at that function for further details */
        Point min(m_sampleToCamera(Point(0, 0, 0))),
                max(m_sampleToCamera(Point(1, 1, 0)));

        m_imageRect.reset();
        m_imageRect.expandBy(Point2(min.x, min.y) / min.z);
        m_imageRect.expandBy(Point2(max.x, max.y) / max.z);
        m_normalization = 1.0f / m_imageRect.getVolume();

        /* Clip-space transformation for OpenGL */
        m_clipTransform = Transform::translate(
                Vector((1-2*relOffset.x)/relSize.x - 1,
                       -(1-2*relOffset.y)/relSize.y + 1, 0.0f)) *
                          Transform::scale(Vector(1.0f / relSize.x, 1.0f / relSize.y, 1.0f));
    }

    /**
     * \brief Compute the directional sensor response function
     * of the camera multiplied with the cosine foreshortening
     * factor associated with the image plane
     *
     * \param d
     *     A normalized direction vector from the aperture position to the
     *     reference point in question (all in local camera space)
     */
    inline Float importance(const Vector &d) const {
        /* How is this derived? Imagine a hypothetical image plane at a
           distance of d=1 away from the pinhole in camera space.

           Then the visible rectangular portion of the plane has the area

              A = (2 * tan(0.5 * xfov in radians))^2 / aspect

           Since we allow crop regions, the actual visible area is
           potentially reduced:

              A' = A * (cropX / filmX) * (cropY / filmY)

           Perspective transformations of such aligned rectangles produce
           an equivalent scaled (but otherwise undistorted) rectangle
           in screen space. This means that a strategy, which uniformly
           generates samples in screen space has an associated area
           density of 1/A' on this rectangle.

           To compute the solid angle density of a sampled point P on
           the rectangle, we can apply the usual measure conversion term:

              d_omega = 1/A' * distance(P, origin)^2 / cos(theta)

           where theta is the angle that the unit direction vector from
           the origin to P makes with the rectangle. Since

              distance(P, origin)^2 = Px^2 + Py^2 + 1

           and

              cos(theta) = 1/sqrt(Px^2 + Py^2 + 1),

           we have

              d_omega = 1 / (A' * cos^3(theta))
        */

        Float cosTheta = Frame::cosTheta(d);

        /* Check if the direction points behind the camera */
        if (cosTheta <= 0)
            return 0.0f;

        /* Compute the position on the plane at distance 1 */
        Float invCosTheta = 1.0f / cosTheta;
        Point2 p(d.x * invCosTheta, d.y * invCosTheta);

        /* Check if the point lies inside the chosen crop rectangle */
        if (!m_imageRect.contains(p))
            return 0.0f;

        return m_normalization * invCosTheta
               * invCosTheta * invCosTheta;
    }

    Spectrum internalEvalDirection(const Vector &d) const{
            Float cosTheta = Frame::cosTheta(d);

            /* Compute the position on the plane at distance 1 */
            Float invCosTheta = 1.0f / cosTheta;
            Point2 uv(d.x * invCosTheta, d.y * invCosTheta);

            /* Check if the point lies inside the chosen crop rectangle */
            if (!m_imageRect.contains(uv)) {
                return Spectrum(0.0f);
            }

            Point samplePoint(uv.x,uv.y,1.0f);
            Point sample = m_cameraToSample(samplePoint * 1.0f);

            /* Convert to fractional pixel coordinates on the specified level */
            Float u = sample.x * m_mapRes.x, v = sample.y * m_mapRes.y;

            /* Take color from the pixel that sample resides */
            int xPos = math::floorToInt(u), yPos = math::floorToInt(v);

            Spectrum value = m_mipmap->evalTexel(0, xPos, yPos);

            return m_normalization * invCosTheta * invCosTheta * invCosTheta * value;
        }

    Spectrum sampleRay(Ray &ray, const Point2 &pixelSample,
                       const Point2 &otherSample, Float timeSample) const {
        ray.time = sampleTime(timeSample);

        /* Compute the corresponding position on the
           near plane (in local camera space) */
        Point nearP = m_sampleToCamera(Point(
                pixelSample.x * m_invResolution.x,
                pixelSample.y * m_invResolution.y, 0.0f));

        Point2 uv(pixelSample.x * m_invResolution.x * m_mapRes.x,pixelSample.y * m_invResolution.y * m_mapRes.y);

        /* Turn that into a normalized ray direction, and
           adjust the ray interval accordingly */
        Vector d = normalize(Vector(nearP));
        Float invZ = 1.0f / d.z;
        ray.mint = m_nearClip * invZ;
        ray.maxt = m_farClip * invZ;

        const Transform &trafo = m_worldTransform->eval(ray.time);
        ray.setOrigin(trafo.transformAffine(Point(0.0f)));
        ray.setDirection(trafo(d));

        return Spectrum(1.0f * m_mipmap->evalTexel(0,math::floorToInt(uv.x),math::floorToInt(uv.y)));
    }

    Spectrum sampleRayDifferential(RayDifferential &ray, const Point2 &pixelSample,
                                   const Point2 &otherSample, Float timeSample) const {
        ray.time = sampleTime(timeSample);

        /* Compute the corresponding position on the
           near plane (in local camera space) */
        Point nearP = m_sampleToCamera(Point(
                pixelSample.x * m_invResolution.x,
                pixelSample.y * m_invResolution.y, 0.0f));

        Point2 uv(pixelSample.x * m_invResolution.x * m_mapRes.x,pixelSample.y * m_invResolution.y * m_mapRes.y);

        /* Turn that into a normalized ray direction, and
           adjust the ray interval accordingly */
        Vector d = normalize(Vector(nearP));
        Float invZ = 1.0f / d.z;
        ray.mint = m_nearClip * invZ;
        ray.maxt = m_farClip * invZ;

        const Transform &trafo = m_worldTransform->eval(ray.time);
        ray.setOrigin(trafo.transformAffine(Point(0.0f)));
        ray.setDirection(trafo(d));
        ray.rxOrigin = ray.ryOrigin = ray.o;

        ray.rxDirection = trafo(normalize(Vector(nearP) + m_dx));
        ray.ryDirection = trafo(normalize(Vector(nearP) + m_dy));
        ray.hasDifferentials = true;

        return Spectrum(1.0f * m_mipmap->evalTexel(0,math::floorToInt(uv.x),math::floorToInt(uv.y)));
    }

    Spectrum samplePosition(PositionSamplingRecord &pRec,
                            const Point2 &sample, const Point2 *extra) const {
        const Transform &trafo = m_worldTransform->eval(pRec.time);
        pRec.p = trafo(Point(0.0f));
        pRec.n = trafo(Vector(0.0f, 0.0f, 1.0f));
        pRec.pdf = 1.0f;
        pRec.measure = EDiscrete;
        return Spectrum(1.0f);
    }

    Spectrum evalPosition(const PositionSamplingRecord &pRec) const {
        return Spectrum((pRec.measure == EDiscrete) ? 1.0f: 0.0f);
    }

    Float pdfPosition(const PositionSamplingRecord &pRec) const {
        return (pRec.measure == EDiscrete) ? 1.0f : 0.0f;
    }

    Spectrum sampleDirection(DirectionSamplingRecord &dRec,
                             PositionSamplingRecord &pRec,
                             const Point2 &sample, const Point2 *extra) const {
        const Transform &trafo = m_worldTransform->eval(pRec.time);

        Point samplePos(sample.x, sample.y, 0.0f);

        if (extra) {
            /* The caller wants to condition on a specific pixel position */
            samplePos.x = (extra->x + sample.x) * m_invResolution.x;
            samplePos.y = (extra->y + sample.y) * m_invResolution.y;
        }

        pRec.uv = Point2(samplePos.x * m_resolution.x,
                         samplePos.y * m_resolution.y);

        /* Compute the corresponding position on the
           near plane (in local camera space) */
        Point nearP = m_sampleToCamera(samplePos);

        /* Turn that into a normalized ray direction */
        Vector d = normalize(Vector(nearP));
        dRec.d = trafo(d);
        dRec.measure = ESolidAngle;
        dRec.pdf = m_normalization / (d.z * d.z * d.z);

        Point2 uv(pRec.uv.x * m_invResolution.x * m_mapRes.x, pRec.uv.y * m_invResolution.y * m_mapRes.y);
        return Spectrum(m_mipmap->evalTexel(0,math::floorToInt(uv.x),math::floorToInt(uv.y)));
    }

    Float pdfDirection(const DirectionSamplingRecord &dRec,
                       const PositionSamplingRecord &pRec) const {
        if (dRec.measure != ESolidAngle)
            return 0.0f;

        const Transform &trafo = m_worldTransform->eval(pRec.time);

        return importance(trafo.inverse()(dRec.d));
    }

    Spectrum evalDirection(const DirectionSamplingRecord &dRec,
                           const PositionSamplingRecord &pRec) const {
        if (dRec.measure != ESolidAngle)
            return Spectrum(0.0f);

        const Transform &trafo = m_worldTransform->eval(pRec.time);

        return internalEvalDirection(trafo.inverse()(dRec.d));
    }

    bool getSamplePosition(const PositionSamplingRecord &pRec,
                           const DirectionSamplingRecord &dRec, Point2 &samplePosition) const {
        Transform invTrafo = m_worldTransform->eval(pRec.time).inverse();
        Point local(Point(invTrafo(dRec.d)));

        if (local.z <= 0)
            return false;

        Point screenSample = m_cameraToSample(local);
        if (screenSample.x < 0 || screenSample.x > 1 ||
            screenSample.y < 0 || screenSample.y > 1)
            return false;

        samplePosition = Point2(
                screenSample.x * m_resolution.x,
                screenSample.y * m_resolution.y);

        return true;
    }

    Spectrum sampleDirect(DirectSamplingRecord &dRec, const Point2 &sample) const {
        const Transform &trafo = m_worldTransform->eval(dRec.time);

        /* Transform the reference point into the local coordinate system */
        Point refP = trafo.inverse().transformAffine(dRec.ref);

        /* Check if it is outside of the clip range */
        if (refP.z < m_nearClip || refP.z > m_farClip) {
            dRec.pdf = 0.0f;
            return Spectrum(0.0f);
        }

        Point screenSample = m_cameraToSample(refP);
        dRec.uv = Point2(screenSample.x, screenSample.y);
        if (dRec.uv.x < 0 || dRec.uv.x > 1 ||
            dRec.uv.y < 0 || dRec.uv.y > 1) {
            dRec.pdf = 0.0f;
            return Spectrum(0.0f);
        }

        dRec.uv.x *= m_resolution.x;
        dRec.uv.y *= m_resolution.y;

        Vector localD(refP);
        Float dist = localD.length(),
                invDist = 1.0f / dist;
        localD *= invDist;

        dRec.p = trafo.transformAffine(Point(0.0f));
        dRec.d = (dRec.p - dRec.ref) * invDist;
        dRec.dist = dist;
        dRec.n = trafo(Vector(0.0f, 0.0f, 1.0f));
        dRec.pdf = 1;
        dRec.measure = EDiscrete;

        Point2 uv(dRec.uv.x * m_invResolution.x * m_mapRes.x, dRec.uv.y * m_invResolution.y * m_mapRes.y);

        return Spectrum(importance(localD) * invDist * invDist *
                m_mipmap->evalTexel(0,math::floorToInt(uv.x),
                math::floorToInt(uv.y)));
    }

    Float pdfDirect(const DirectSamplingRecord &dRec) const {
        return (dRec.measure == EDiscrete) ? 1.0f : 0.0f;
    }

    Transform getProjectionTransform(const Point2 &apertureSample,
                                     const Point2 &aaSample) const {
        Float right = std::tan(m_xfov * M_PI/360) * m_nearClip, left = -right;
        Float top = right / m_aspect, bottom = -top;

        Vector2 offset(
                (right-left)/m_film->getSize().x * (aaSample.x-0.5f),
                (top-bottom)/m_film->getSize().y * (aaSample.y-0.5f));

        return m_clipTransform *
               Transform::glFrustum(left+offset.x, right+offset.x,
                                    bottom+offset.y, top+offset.y, m_nearClip, m_farClip);
    }

    AABB getAABB() const {
        return m_worldTransform->getTranslationBounds();
    }

    std::string toString() const {
        std::ostringstream oss;
        oss << "CodedPerspectiveCamera[" << endl
            << "  fov = [" << getXFov() << ", " << getYFov() << "]," << endl
            << "  nearClip = " << m_nearClip << "," << endl
            << "  farClip = " << m_farClip << "," << endl
            << "  worldTransform = " << indent(m_worldTransform.toString()) << "," << endl
            << "  sampler = " << indent(m_sampler->toString()) << "," << endl
            << "  film = " << indent(m_film->toString()) << "," << endl
            << "  medium = " << indent(m_medium.toString()) << "," << endl
            << "  shutterOpen = " << m_shutterOpen << "," << endl
            << "  shutterOpenTime = " << m_shutterOpenTime << endl
            << "]";
        return oss.str();
    }

    MTS_DECLARE_CLASS()
private:
    Transform m_cameraToSample;
    Transform m_sampleToCamera;
    Transform m_clipTransform;
    AABB2 m_imageRect;
    Float m_normalization;
    Vector m_dx, m_dy;
};

MTS_IMPLEMENT_CLASS_S(CodedPerspectiveCameraImpl, false, CodedPerspectiveCamera)
MTS_EXPORT_PLUGIN(CodedPerspectiveCameraImpl, "Coded perspective camera");
MTS_NAMESPACE_END
