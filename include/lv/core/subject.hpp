#pragma once

/**
 * @file subject.hpp
 * @brief Reactive state in two forms: one that owns its subject, one that borrows.
 *
 * Subject<T> owns an lv_subject_t and initialises it in place. No heap allocation, and the
 * subject lives exactly as long as the Subject does - which is what makes it usable for
 * application state declared as a member.
 *
 * SubjectRef<T> borrows one. It is built from an lv_subject_t* and owns nothing, so it wraps a
 * subject created elsewhere: one the LVGL Pro export generated, for instance, where the
 * generator allocates the subject (lv_subject_create) and hands out the pointer. Without
 * this form there is no way to use such a subject through the wrapper at all, and the only
 * alternatives are to reach for the raw C API or to keep a second subject of your own and
 * copy values between the two.
 *
 * Both speak the same interface, provided once by SubjectMixin and reached through the
 * derived class, so the two cannot drift apart. A class using it must provide
 *
 *     lv_subject_t* subject() noexcept;
 *
 * SubjectRef is pointer-sized. Subject is one lv_subject_t plus its value - unchanged from when
 * it was the only form, because the mixin holds nothing and the empty base costs nothing.
 */

#include <lvgl.h>
#include <type_traits>
#include <cstdint>
#include <cstring>
#include <string>

// Only available if LVGL observer is enabled
#if LV_USE_OBSERVER

namespace lv {

/**
 * @brief The reactive interface, shared by State and SubjectRef
 *
 * Holds no data: every access goes through the derived class's own subject(), so the base
 * contributes nothing to the object's size (empty base optimisation).
 *
 * @tparam Derived The class providing lv_subject_t* subject()
 * @tparam T Value type (int32_t, lv_color_t, pointer types supported)
 */
template<typename Derived, typename T>
class SubjectMixin {
private:
    [[nodiscard]] Derived& derived() noexcept { return static_cast<Derived&>(*this); }
    [[nodiscard]] const Derived& derived() const noexcept { return static_cast<const Derived&>(*this); }

public:
    /**
     * @brief The lv_subject_t behind this handle
     *
     * Non-const, because every lv_subject_* API takes a non-const pointer - including the
     * ones that only read. It is what the widget bind helpers need, so that they can accept a
     * handle they will not write to. The cast is safe in that direction: nothing here writes
     * through it, and the writes in this class name derived().subject() themselves.
     */
    [[nodiscard]] lv_subject_t* handle() const noexcept {
        return const_cast<lv_subject_t*>(derived().subject());
    }
    // ==================== Accessors ====================

    /// Get current value. A handle with no subject yet reads as T{}.
    [[nodiscard]] T get() const noexcept {
        if (handle() == nullptr) {
            return T{};
        }

        if constexpr (std::is_integral_v<T> && sizeof(T) <= sizeof(int32_t)) {
            return static_cast<T>(lv_subject_get_int(handle()));
        }

        if constexpr (std::is_floating_point_v<T> && sizeof(T) <= sizeof(float)) {
            return static_cast<T>(lv_subject_get_float(handle()));
        }

        if constexpr (std::is_same_v<T, lv_color_t>) {
            return lv_subject_get_color(handle());
        }

        if constexpr (std::is_same_v<std::remove_const_t<T>, char*>) {
            return static_cast<T>(lv_subject_get_string(handle()));
        }

        if constexpr (std::is_pointer_v<T>) {
            return static_cast<T>(lv_subject_get_pointer(handle()));
        }

        return T{};
    }

    /// Get value by implicit conversion
    [[nodiscard]] operator T() const noexcept {
        return get();
    }

    /// Set new value (notifies observers if changed). Writing to a handle with no subject
    /// yet is ignored, which is what lets one be given its subject later.
    void set(const T& new_value) noexcept {
        if (handle() == nullptr) {
            return;
        }
        if constexpr (std::is_integral_v<T> && sizeof(T) <= sizeof(int32_t)) {
            lv_subject_set_int(derived().subject(), static_cast<int32_t>(new_value));
        } else if constexpr (std::is_floating_point_v<T> && sizeof(T) <= sizeof(float)) {
            lv_subject_set_float(derived().subject(), static_cast<float>(new_value));
        } else if constexpr (std::is_same_v<T, std::string>) {
            lv_subject_copy_string(derived().subject(), new_value.c_str());
        } else if constexpr (std::is_pointer_v<T>) {
            lv_subject_set_pointer(derived().subject(), static_cast<void*>(new_value));
        } else if constexpr (std::is_same_v<T, lv_color_t>) {
            lv_subject_set_color(derived().subject(), new_value);
        }
    }

    /// Force notify all observers (even if value unchanged)
    void notify() noexcept {
        lv_subject_notify(handle());
    }

    // ==================== Modifiers (for numeric types) ====================

    /// Increment value (for integral types)
    template<typename U = T>
        requires std::is_arithmetic_v<U>
    void increment(U delta = 1) noexcept {
        set(get() + delta);
    }

    /// Decrement value (for integral types)
    template<typename U = T>
        requires std::is_arithmetic_v<U>
    void decrement(U delta = 1) noexcept {
        set(get() - delta);
    }

    /// Prefix increment
    template<typename U = T>
        requires std::is_arithmetic_v<U>
    Derived& operator++() noexcept {
        increment();
        return derived();
    }

    /// Prefix decrement
    template<typename U = T>
        requires std::is_arithmetic_v<U>
    Derived& operator--() noexcept {
        decrement();
        return derived();
    }

    /// Add and assign
    template<typename U = T>
        requires std::is_arithmetic_v<U>
    Derived& operator+=(U delta) noexcept {
        increment(delta);
        return derived();
    }

    /// Subtract and assign
    template<typename U = T>
        requires std::is_arithmetic_v<U>
    Derived& operator-=(U delta) noexcept {
        decrement(delta);
        return derived();
    }

    // ==================== LVGL Integration ====================

    /**
     * @brief Add observer with member function callback (zero-allocation)
     *
     * Uses LVGL's user_data to store instance pointer, callback is resolved
     * at compile time via template parameter.
     *
     * Supported types:
     * - Integral types <= 32 bits (int, short, bool, etc.)
     * - lv_color_t
     * - Pointer types
     * - Other trivially copyable types (accessed via pointer to stored value)
     *
     * @tparam MemFn Pointer to member function void(T) or void(const T&)
     * @param instance Pointer to object instance
     */
    template<auto MemFn, typename Instance>
        requires std::is_member_function_pointer_v<decltype(MemFn)>
    lv_observer_t* observe(Instance* instance) const noexcept {
        return lv_subject_add_observer(handle(),
            [](lv_observer_t* observer, lv_subject_t*) {
                auto* inst = static_cast<Instance*>(lv_observer_get_user_data(observer));
                (inst->*MemFn)();
            }, instance);
    }

    /**
     * @brief Add observer with C-style callback
     *
     * For direct LVGL integration when you need raw observer access.
     *
     * @param cb LVGL observer callback
     * @param user_data User data passed to callback
     */
    lv_observer_t* observe_raw(lv_observer_cb_t cb, void* user_data = nullptr) const noexcept {
        return lv_subject_add_observer(handle(), cb, user_data);
    }

    /**
     * @brief Add observer tied to an LVGL object's lifecycle
     *
     * The observer is automatically removed when the target object is deleted.
     * This is useful for UI bindings where the observer should live as long as the widget.
     *
     * @param cb LVGL observer callback
     * @param target_obj The LVGL object to tie the observer's lifecycle to
     * @param user_data User data passed to callback
     */
    lv_observer_t* observe_obj(lv_observer_cb_t cb, lv_obj_t* target_obj, void* user_data = nullptr) const noexcept {
        return lv_subject_add_observer_obj(handle(), cb, target_obj, user_data);
    }

    /**
     * @brief Add observer tied to an ObjectView's lifecycle
     *
     * Convenience overload accepting ObjectView instead of raw pointer.
     */
    template<typename ObjType>
        requires requires(ObjType o) { { o.get() } -> std::convertible_to<lv_obj_t*>; }
    lv_observer_t* observe_obj(lv_observer_cb_t cb, ObjType target_obj, void* user_data = nullptr) const noexcept {
        return lv_subject_add_observer_obj(handle(), cb, target_obj.get(), user_data);
    }
};

/**
 * @brief Reactive state container that owns its subject (stack-allocated)
 *
 * Wraps LVGL's lv_subject_t for reactive data binding. When state changes, all bound
 * widgets automatically update.
 *
 * Size: sizeof(lv_subject_t) + sizeof(T) - approximately 48-56 bytes
 * Heap allocation: NONE
 *
 * @tparam T Value type (int32_t, lv_color_t, pointer types supported)
 */
template<typename T>
class Subject : public SubjectMixin<Subject<T>, T> {
private:
    lv_subject_t m_subject;

public:
    /// Construct with initial value
    explicit Subject(const T& initial = T{}) noexcept {
        if constexpr (std::is_integral_v<T> && sizeof(T) <= sizeof(int32_t)) {
            lv_subject_init_int(&m_subject, static_cast<int32_t>(initial));
        } else if constexpr (std::is_floating_point_v<T> && sizeof(T) <= sizeof(float)) {
            lv_subject_init_float(&m_subject, static_cast<float>(initial));
        } else if constexpr (std::is_same_v<T, std::string>) {
            // Ugly, but it works for now...
            lv_subject_init_string(&m_subject, new char[16], new char[16], 16, initial.c_str());
        } else if constexpr (std::is_pointer_v<T>) {
            lv_subject_init_pointer(&m_subject, static_cast<void*>(initial), T());
        } else if constexpr (std::is_same_v<T, lv_color_t>) {
            lv_subject_init_color(&m_subject, initial);
        }
    }

    /// Destructor
    ~Subject() noexcept {
        lv_subject_deinit(&m_subject);
    }

    // Non-copyable (subjects can't be copied)
    Subject(const Subject&) = delete;
    Subject& operator=(const Subject&) = delete;

    // Non-movable (subjects have internal pointers)
    Subject(Subject&&) = delete;
    Subject& operator=(Subject&&) = delete;

    using ValueType = T;

    /// The subject this state owns
    [[nodiscard]] lv_subject_t* subject() noexcept { return &m_subject; }
    [[nodiscard]] const lv_subject_t* subject() const noexcept { return &m_subject; }
};

/**
 * @brief Non-owning handle on a subject created elsewhere
 *
 * Built from a pointer, owns nothing and frees nothing. Use it for a subject this code did
 * not create - one the LVGL Pro export generated, or one an object in C handed over. The
 * subject must outlive the handle; the handle does not extend its life.
 *
 * Size: sizeof(void*) - the interface it exposes lives in SubjectMixin.
 *
 * @tparam T Value type, which must match the subject's own type
 */
template<typename T>
class SubjectRef : public SubjectMixin<SubjectRef<T>, T> {
private:
    lv_subject_t* m_subject;

public:
    /// Borrow an existing subject. A null subject is allowed and reads as T{}.
    constexpr SubjectRef(lv_subject_t* subject) noexcept : m_subject(subject) {}

    /// Borrow the subject owned by a Subject of the same type
    constexpr SubjectRef(Subject<T>& owned) noexcept : m_subject(owned.subject()) {}

    using ValueType = T;

    /// The subject being borrowed
    [[nodiscard]] lv_subject_t* subject() noexcept { return m_subject; }
    [[nodiscard]] const lv_subject_t* subject() const noexcept { return m_subject; }
};


// ==================== Specialized Subject Types ====================

/// Integer state (most common)
using IntSubject = Subject<int32_t>;

/// Boolean state
using BoolSubject = Subject<bool>;

/// Color state
using ColorSubject = Subject<lv_color_t>;

// ==================== Zero-Cost Verification ====================

// Static assertions to verify Subject<T> overhead is minimal
// Subject must be exactly lv_subject_t + T (no vtable, no extra padding beyond alignment)
static_assert(sizeof(IntSubject) <= sizeof(lv_subject_t) + sizeof(int32_t) + alignof(lv_subject_t),
    "IntSubject should have minimal overhead over lv_subject_t + value");
static_assert(sizeof(BoolSubject) <= sizeof(lv_subject_t) + sizeof(bool) + alignof(lv_subject_t),
    "BoolSubject should have minimal overhead over lv_subject_t + value");
static_assert(sizeof(ColorSubject) <= sizeof(lv_subject_t) + sizeof(lv_color_t) + alignof(lv_subject_t),
    "ColorSubject should have minimal overhead over lv_subject_t + value");

// A borrowing handle must cost one pointer and nothing else
static_assert(sizeof(SubjectRef<int32_t>) == sizeof(void*),
    "SubjectRef must be exactly pointer-sized");

// ==================== Observer Helper Functions ====================

/// Get target object from observer (for use in observer callbacks)
[[nodiscard]] inline lv_obj_t* observer_get_target_obj(lv_observer_t* observer) noexcept {
    return lv_observer_get_target_obj(observer);
}

/// Get target (void*) from observer (for use in observer callbacks)
[[nodiscard]] inline void* observer_get_target(lv_observer_t* observer) noexcept {
    return lv_observer_get_target(observer);
}

/// Get user data from observer (for use in observer callbacks)
[[nodiscard]] inline void* observer_get_user_data(lv_observer_t* observer) noexcept {
    return lv_observer_get_user_data(observer);
}

/// Get user data from observer with typed cast
template<typename T>
[[nodiscard]] inline T* observer_get_user_data(lv_observer_t* observer) noexcept {
    return static_cast<T*>(lv_observer_get_user_data(observer));
}

/// Get int value from subject (for use in observer callbacks)
[[nodiscard]] inline int32_t subject_get_int(lv_subject_t* subject) noexcept {
    return lv_subject_get_int(subject);
}

/// Get int value from subject (for use in observer callbacks)
[[nodiscard]] inline float subject_get_float(lv_subject_t* subject) noexcept {
    return lv_subject_get_float(subject);
}

/// Get color value from subject (for use in observer callbacks)
[[nodiscard]] inline lv_color_t subject_get_color(lv_subject_t* subject) noexcept {
    return lv_subject_get_color(subject);
}

/// Get pointer from subject (for use in observer callbacks)
[[nodiscard]] inline const void* subject_get_pointer(lv_subject_t* subject) noexcept {
    return lv_subject_get_pointer(subject);
}

/// Get pointer from subject with typed cast
template<typename T>
[[nodiscard]] inline T* subject_get_pointer(lv_subject_t* subject) noexcept {
    return const_cast<T*>(static_cast<const T*>(lv_subject_get_pointer(subject)));
}

} // namespace lv

#endif // LV_USE_OBSERVER
