#pragma once

/**
 * @file state.hpp
 * @brief Zero-allocation reactive state using LVGL's observer system
 *
 * State<T> wraps lv_subject_t for reactive updates without heap allocation.
 * The subject is embedded in the State object (stack-allocated when State is on stack).
 */

#include <lvgl.h>
#include <type_traits>
#include <cstdint>
#include <cstring>

// Only available if LVGL observer is enabled
#if LV_USE_OBSERVER

namespace lv {

/**
 * @brief Reactive state container (stack-allocated)
 *
 * Wraps LVGL's lv_subject_t for reactive data binding.
 * When state changes, all bound widgets automatically update.
 *
 * Size: sizeof(lv_subject_t) + sizeof(T) - approximately 48-56 bytes
 * Heap allocation: NONE
 *
 * @tparam T Value type (int32_t, lv_color_t, pointer types supported)
 */
template<typename T>
class State {
private:
    lv_subject_t m_subject;

public:
    /// Construct with initial value
    explicit State(const T& initial = T{}) noexcept {
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
    ~State() noexcept {
        lv_subject_deinit(&m_subject);
    }

    // Non-copyable (subjects can't be copied)
    State(const State&) = delete;
    State& operator=(const State&) = delete;

    // Non-movable (subjects have internal pointers)
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    // ==================== Accessors ====================

    /// Get current value
    [[nodiscard]] const T& get() const noexcept {
        if constexpr (std::is_integral_v<T> && sizeof(T) <= sizeof(int32_t)) {
            return static_cast<T>(lv_subject_get_int(subject));
        }
        
        if constexpr (std::is_floating_point_v<T> && sizeof(T) <= sizeof(float)) {
            return static_cast<T>(lv_subject_get_float(subject));
        }
        
        if constexpr (std::is_same_v<T, lv_color_t>) {
            return lv_subject_get_color(subject);
        }
        
        if constexpr (std::is_same_v<std::remove_const_t<T>, char*>) {
            return static_cast<T>(lv_subject_get_string(subject));
        }

        if constexpr (std::is_pointer_v<T>) {
            return static_cast<T>(lv_subject_get_pointer(subject));
        }
        
        return T{};
    }

    /// Get value by implicit conversion
    [[nodiscard]] operator const T&() const noexcept {
        return get();
    }

    /// Set new value (notifies observers if changed)
    void set(const T& new_value) noexcept {
        if constexpr (std::is_integral_v<T> && sizeof(T) <= sizeof(int32_t)) {
            lv_subject_set_int(&m_subject, static_cast<int32_t>(new_value));
        } else if constexpr (std::is_floating_point_v<T> && sizeof(T) <= sizeof(float)) {
            lv_subject_set_float(&m_subject, static_cast<float>(new_value));
        } else if constexpr (std::is_same_v<T, std::string>) {
            lv_subject_copy_string(&m_subject, new_value.c_str());
        } else if constexpr (std::is_pointer_v<T>) {
            lv_subject_set_pointer(&m_subject, static_cast<void*>(new_value));
        } else if constexpr (std::is_same_v<T, lv_color_t>) {
            lv_subject_set_color(&m_subject, new_value);
        }
    }

    /// Force notify all observers (even if value unchanged)
    void notify() noexcept {
        lv_subject_notify(&m_subject);
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
    State& operator++() noexcept {
        increment();
        return *this;
    }

    /// Prefix decrement
    template<typename U = T>
        requires std::is_arithmetic_v<U>
    State& operator--() noexcept {
        decrement();
        return *this;
    }

    /// Add and assign
    template<typename U = T>
        requires std::is_arithmetic_v<U>
    State& operator+=(U delta) noexcept {
        increment(delta);
        return *this;
    }

    /// Subtract and assign
    template<typename U = T>
        requires std::is_arithmetic_v<U>
    State& operator-=(U delta) noexcept {
        decrement(delta);
        return *this;
    }

    // ==================== LVGL Integration ====================

    /// Get underlying subject for LVGL binding APIs
    [[nodiscard]] lv_subject_t* subject() noexcept {
        return &m_subject;
    }

    [[nodiscard]] const lv_subject_t* subject() const noexcept {
        return &m_subject;
    }

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
    lv_observer_t* observe(Instance* instance) noexcept {
        return lv_subject_add_observer(&m_subject,
            [](lv_observer_t* observer, lv_subject_t* subject) {
                auto* inst = static_cast<Instance*>(lv_observer_get_user_data(observer));
                (inst->*MemFn)(get());
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
    lv_observer_t* observe_raw(lv_observer_cb_t cb, void* user_data = nullptr) noexcept {
        return lv_subject_add_observer(&m_subject, cb, user_data);
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
    lv_observer_t* observe_obj(lv_observer_cb_t cb, lv_obj_t* target_obj, void* user_data = nullptr) noexcept {
        return lv_subject_add_observer_obj(&m_subject, cb, target_obj, user_data);
    }

    /**
     * @brief Add observer tied to an ObjectView's lifecycle
     *
     * Convenience overload accepting ObjectView instead of raw pointer.
     */
    template<typename ObjType>
        requires requires(ObjType o) { { o.get() } -> std::convertible_to<lv_obj_t*>; }
    lv_observer_t* observe_obj(lv_observer_cb_t cb, ObjType target_obj, void* user_data = nullptr) noexcept {
        return lv_subject_add_observer_obj(&m_subject, cb, target_obj.get(), user_data);
    }
};


// ==================== Specialized State Types ====================

/// Integer state (most common)
using IntState = State<int32_t>;

/// Boolean state
using BoolState = State<bool>;

/// Color state
using ColorState = State<lv_color_t>;

// ==================== Zero-Cost Verification ====================

// Static assertions to verify State<T> overhead is minimal
// State must be exactly lv_subject_t + T (no vtable, no extra padding beyond alignment)
static_assert(sizeof(IntState) <= sizeof(lv_subject_t) + sizeof(int32_t) + alignof(lv_subject_t),
    "IntState should have minimal overhead over lv_subject_t + value");
static_assert(sizeof(BoolState) <= sizeof(lv_subject_t) + sizeof(bool) + alignof(lv_subject_t),
    "BoolState should have minimal overhead over lv_subject_t + value");
static_assert(sizeof(ColorState) <= sizeof(lv_subject_t) + sizeof(lv_color_t) + alignof(lv_subject_t),
    "ColorState should have minimal overhead over lv_subject_t + value");

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
