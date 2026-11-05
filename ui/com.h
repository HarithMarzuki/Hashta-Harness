// A COM smart pointer with just enough surface for this app.
#pragma once

template <class T>
struct Com {
    T* p = nullptr;

    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;

    Com(Com&& other) noexcept : p(other.p) { other.p = nullptr; }
    Com& operator=(Com&& other) noexcept {
        if (this != &other) {
            reset();
            p = other.p;
            other.p = nullptr;
        }
        return *this;
    }

    ~Com() { reset(); }

    void reset() {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }

    T** out() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};
