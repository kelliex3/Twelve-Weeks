// ported from tools/ext/t2r-ext.c (WorldRayKick / RayT / LocAt / SglCam)

#include "raycast.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/script.hpp"

#include <bit>
#include <cmath>

namespace engine::raycast {

    namespace {

        constexpr uint64_t kInvalid = static_cast<uint64_t>(-1);

        // A non-finite / absurd point never reaches the engine (ext: RayBad).
        bool Bad(const Vector3& p)
        {
            return !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)
                || std::fabs(p.x) > 1e6f || std::fabs(p.y) > 1e6f || std::fabs(p.z) > 1e6f;
        }

        // offset-locator at world point `p`, expressed in the camera locator's frame:
        // conj(camRot) rotate (p - camPos). 0 on failure (ext: LocAt).
        uint64_t LocAt(uint64_t cam, const Vector3& camPos, const Quaternion& camRot,
                       const Vector3& p)
        {
            const Vector3 d = p - camPos;
            const Quaternion conj(-camRot.x, -camRot.y, -camRot.z, camRot.w);
            const uint64_t dv = MEMORY::ALLOC_VECTOR(conj.rotate(d));
            if (!dv)
                return 0;
            const uint64_t r = LOCATOR::OFFSET_LOCATOR(cam, dv);
            return r == kInvalid ? 0 : r;
        }

    }

    uint64_t CameraLocator()
    {
        const uint64_t cam = DC::Call<"camera-through-reticle-locator">();
        return (cam && cam != kInvalid) ? cam : 0;
    }

    bool CameraPosition(Vector3& out)
    {
        const uint64_t cam = CameraLocator();
        if (!cam)
            return false;
        const Vector3* p = DC::Call<"get-locator-position", const Vector3>(cam);
        if (!p)
            return false;
        out = *p;
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }

    bool CameraRotation(Quaternion& out)
    {
        const uint64_t cam = CameraLocator();
        if (!cam)
            return false;
        const Quaternion* q = DC::Call<"get-locator-rotation", const Quaternion>(cam);
        if (!q)
            return false;
        out = *q;
        return std::isfinite(out.x) && std::isfinite(out.y)
            && std::isfinite(out.z) && std::isfinite(out.w);
    }

    uint64_t KickWorldRay(const Vector3& from, const Vector3& to, uint32_t mask, uint64_t ignore)
    {
        if (Bad(from) || Bad(to))
            return 0;

        const uint64_t cam = CameraLocator();
        if (!cam)
            return 0;

        Vector3 c;
        Quaternion q;
        if (!CameraPosition(c) || !CameraRotation(q))
            return 0;

        // The engine's own cast-start guard: refuse non-finite / huge input here.
        if (!(std::fabs(c.x) < 1e7f) || !(std::fabs(c.y) < 1e7f) || !(std::fabs(c.z) < 1e7f))
            return 0;
        if (!(std::fabs(q.x) <= 1.01f) || !(std::fabs(q.y) <= 1.01f)
            || !(std::fabs(q.z) <= 1.01f) || !(std::fabs(q.w) <= 1.01f))
            return 0;

        const uint64_t la = LocAt(cam, c, q, from);
        const uint64_t lb = LocAt(cam, c, q, to);
        if (!la || !lb)
            return 0;

        const uint64_t j = LOCATOR::KICK_RAYCAST(la, lb, ignore ? ignore : SID("player"), mask);
        return j == kInvalid ? 0 : j;
    }

    float RayT(uint64_t job)
    {
        if (!job)
            return -2.0f;
        const uint64_t r = DC::Call<"wait-raycast-t">(job);
        if (r == kInvalid)
            return -1.0f;
        return std::bit_cast<float>(static_cast<uint32_t>(r));
    }

}
