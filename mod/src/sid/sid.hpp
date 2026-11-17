#pragma once

#include <cstdint>
#include <cmath>

#define _rgba(r,g,b,a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define _rgb(r,g,b) _rgba(r,g,b,255)

typedef uint64_t StringId64;

constexpr StringId64 ToStringId64(const char* str)
{
    uint64_t base = 0xCBF29CE484222325;
    if (*str)
    {
        while (*str)
        {
            base = 0x100000001B3 * (base ^ *str++);
        }
    }
    return base;
}

#define SID(str) ToStringId64(str)

typedef struct Vector2
{
    float x;
    float y;
} Vector2;

struct Vector3 {
    float x, y, z;

    Vector3() : x(0), y(0), z(0) {}

    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vector3 operator+(const Vector3& other) const {
        return Vector3(x + other.x, y + other.y, z + other.z);
    }

    Vector3 operator-(const Vector3& other) const {
        return Vector3(x - other.x, y - other.y, z - other.z);
    }

    Vector3 operator*(float scalar) const {
        return Vector3(x * scalar, y * scalar, z * scalar);
    }

    Vector3 operator/(float scalar) const {
        return Vector3(x / scalar, y / scalar, z / scalar);
    }

    inline float hdistance(const Vector3& other) const {
        const float _x = x - other.x;
        const float _z = z - other.z;
        return std::sqrt(_x * _x + _z * _z);
    }

    inline float distance(const Vector3& other) const {
        const float _x = x - other.x;
        const float _y = y - other.y;
        const float _z = z - other.z;
        return std::sqrt(_x * _x + _y * _y + _z * _z);
    }
};

struct Quaternion {
    float x, y, z, w;

    Quaternion() : x(0), y(0), z(0), w(1) {}
    Quaternion(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    Quaternion operator*(const Quaternion& other) const {
        return {
            w * other.x + x * other.w + y * other.z - z * other.y,
            w * other.y - x * other.z + y * other.w + z * other.x,
            w * other.z + x * other.y - y * other.x + z * other.w,
            w * other.w - x * other.x - y * other.y - z * other.z
        };
    }

    Vector3 rotate(const Vector3& v) const {
        Quaternion vecQuat(v.x, v.y, v.z, 0.0f);
        Quaternion conj(-x, -y, -z, w);
        Quaternion result = (*this) * vecQuat * conj;

        return { result.x, result.y, result.z };
    }

    static Quaternion Identity() {
        return Quaternion(0, 0, 0, 1);
    }

    static Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t) {
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;

        Quaternion end = b;
        if (dot < 0.0f) {
            dot = -dot;
            end = Quaternion(-b.x, -b.y, -b.z, -b.w);
        }

        const float THRESHOLD = 0.9995f;
        if (dot > THRESHOLD) {
            Quaternion result = Quaternion(
                a.x + t * (end.x - a.x),
                a.y + t * (end.y - a.y),
                a.z + t * (end.z - a.z),
                a.w + t * (end.w - a.w)
            );
            float length = std::sqrt(result.x * result.x + result.y * result.y + result.z * result.z + result.w * result.w);
            return Quaternion(result.x / length, result.y / length, result.z / length, result.w / length);
        }

        float theta = std::acos(dot);
        float sinTheta = std::sqrt(1.0f - dot * dot);

        float weightA = std::sin((1.0f - t) * theta) / sinTheta;
        float weightB = std::sin(t * theta) / sinTheta;

        return Quaternion(
            weightA * a.x + weightB * end.x,
            weightA * a.y + weightB * end.y,
            weightA * a.z + weightB * end.z,
            weightA * a.w + weightB * end.w
        );
    }
};

struct Transform {
    Vector3 Position;
    Quaternion Rotation;

    Transform() : Position(), Rotation() {}
    Transform(const Vector3& pos, const Quaternion& rot) : Position(pos), Rotation(rot) {}

    inline float hdistance(const Transform& other) const {
        return Position.hdistance(other.Position);
    }

    inline float distance(const Transform& other) const {
        return Position.distance(other.Position);
    }
};

struct Gui2PrimitiveDrawRec
{
    uint32_t rgba;
    float x1;
    float y1;
    float x2;
    float y2;
};

template<typename T>
T customMin(T a, T b) {
    return (a < b) ? a : b;
}

template<typename T>
T customMax(T a, T b) {
    return (a > b) ? a : b;
}
