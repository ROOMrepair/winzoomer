#pragma once

template <typename T>
struct Vec2 {
  T x, y;
  Vec2() : x(0), y(0) {}
  Vec2(T _x, T _y) : x(_x), y(_y) {}
  Vec2 operator+(const T& s) const { return Vec2(x + s, y + s); }
  Vec2 operator+(const Vec2& other) const {
    return Vec2(x + other.x, y + other.y);
  }
  Vec2 operator-(const T& s) const { return Vec2(x - s, y - s); }
  Vec2 operator-(const Vec2& other) const {
    return Vec2(x - other.x, y - other.y);
  }
  Vec2 operator*(const T& s) const { return Vec2(x * s, y * s); }
  Vec2 operator/(const T& s) const { return Vec2(x / s, y / s); }
  Vec2& operator+=(const Vec2& v) {
    this->x += v.x;
    this->y += v.y;
    return *this;
  }
  Vec2& operator-=(const Vec2& v) {
    this->x -= v.x;
    this->y -= v.y;
    return *this;
  }
  float length() { return static_cast<T>(sqrt(x * x + y * y)); }
};

template <typename T>
struct Vec3 {
  Vec3(T _x, T _y, T _z) : x(_x), y(_y), z(_z) {}
  T x, y, z;
};

struct Mat4 {
  float m[16];

  static Mat4 identity() {
    Mat4 r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
  }
};


inline Mat4 ortho(float left, float right, float bottom, float top) {
  Mat4 r = {};

  r.m[0] = 2.0f / (right - left);
  r.m[5] = 2.0f / (top - bottom);
  r.m[10] = -1.0f;
  r.m[12] = -(right + left) / (right - left);
  r.m[13] = -(top + bottom) / (top - bottom);
  r.m[15] = 1.0f;

  return r;
}

typedef Vec2<float> Vec2f;
typedef Vec2<int> Vec2i;
typedef Vec3<int> Vec3i;
typedef Vec3<float> Vec3f;
