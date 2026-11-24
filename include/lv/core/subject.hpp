#pragma once

/**
 * @file subject.hpp
 * @brief Reactive state in two forms: one that owns its subject, one that borrows.
 *
 * Subject<T> creates its subject with lv_subject_create and deletes it in the destructor, so the
 * subject lives exactly as long as the Subject does. That is a heap allocation, and the subject sits
 * on LVGL's own list of them: the create/delete pair is what LVGL offers, and the in-place form it
 * replaces is deprecated. The subject is made in the constructor, so a Subject must be constructed
 * after lv_init - it cannot be a global.
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
 * A Subject whose creation failed holds no subject at all, which is a state the interface
 * already allows: a handle with none reads as T{} and ignores writes.
 *
 * Subject is a pointer and its value; SubjectRef is the pointer alone.
 */

#include <lvgl.h>
#include <array>
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
            // Writes into the buffers the subject's owner set up; a string subject created by
            // Subject<T> has its own, and one borrowed through SubjectRef has whatever the code that
            // made it gave it.
            lv_subject_set_string(derived().subject(), new_value.c_str());
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
 * @brief Where a subject's own buffers live, when its type has any
 *
 * A string subject does not keep its text: it writes through pointers its owner gives it, and those
 * must outlive it (see lv_subject_set_string_buffer_static). They are members of the handle, in this
 * base, so the pointers cannot dangle and nothing is allocated for them. Deleting a subject frees
 * the subject and its observers, never buffers it was handed.
 *
 * The size is the whole of a string Subject's text, and a longer initial value is truncated into it.
 *
 * Every other value type has none, and this base is empty for them, which costs nothing. The subject
 * holds the pointers, so a handle must not move - which is why Subject is neither copyable nor
 * movable.
 */
template<typename T>
struct SubjectStorage {};

template<>
struct SubjectStorage<std::string> {
    static constexpr size_t kStringSize = 16;
    std::array<char, kStringSize> value{};
    std::array<char, kStringSize> previous{};
};

/**
 * @brief Reactive state container that owns its subject
 *
 * Wraps LVGL's lv_subject_t for reactive data binding. When state changes, all bound
 * widgets automatically update.
 *
 * Size: a pointer, plus whatever buffers the value type needs - 32 bytes of them for std::string,
 * and none for the rest, where the storage base is empty.
 * Heap allocation: the subject itself, on LVGL's list, made by lv_subject_create
 *
 * @tparam T Value type (int32_t, lv_color_t, pointer types supported)
 */
template<typename T>
class Subject : public SubjectMixin<Subject<T>, T>, public SubjectStorage<T> {
private:
    lv_subject_t* m_subject = nullptr;

public:
    /// Construct with initial value
    explicit Subject(const T& initial = T{}) noexcept {
        if constexpr (std::is_integral_v<T> && sizeof(T) <= sizeof(int32_t)) {
            m_subject = lv_subject_create(LV_SUBJECT_TYPE_INT);
            if (m_subject) lv_subject_set_int(m_subject, static_cast<int32_t>(initial));
        } else if constexpr (std::is_floating_point_v<T> && sizeof(T) <= sizeof(float)) {
            m_subject = lv_subject_create(LV_SUBJECT_TYPE_FLOAT);
            if (m_subject) lv_subject_set_float(m_subject, static_cast<float>(initial));
        } else if constexpr (std::is_same_v<T, std::string>) {
            auto& buffers = static_cast<SubjectStorage<std::string>&>(*this);
            m_subject = lv_subject_create(LV_SUBJECT_TYPE_STRING);
            if (m_subject) {
                lv_subject_set_string_buffer_static(m_subject, buffers.value.data(),
                                                    buffers.previous.data(), buffers.value.size());
                lv_subject_set_string(m_subject, initial.c_str());
            }
        } else if constexpr (std::is_pointer_v<T>) {
            m_subject = lv_subject_create(LV_SUBJECT_TYPE_POINTER);
            if (m_subject) lv_subject_set_pointer(m_subject, static_cast<void*>(initial));
        } else if constexpr (std::is_same_v<T, lv_color_t>) {
            m_subject = lv_subject_create(LV_SUBJECT_TYPE_COLOR);
            if (m_subject) lv_subject_set_color(m_subject, initial);
        }
    }

    /// Destructor
    ~Subject() noexcept {
        lv_subject_delete(m_subject);
    }

    // Non-copyable (subjects can't be copied)
    Subject(const Subject&) = delete;
    Subject& operator=(const Subject&) = delete;

    // Non-movable (subjects have internal pointers)
    Subject(Subject&&) = delete;
    Subject& operator=(Subject&&) = delete;

    using ValueType = T;

    /// The subject this state owns
    [[nodiscard]] lv_subject_t* subject() noexcept { return m_subject; }
    [[nodiscard]] const lv_subject_t* subject() const noexcept { return m_subject; }
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

// ==================== Handle Size Verification ====================

// Static assertions on the handles' cost. A Subject is a pointer to its subject and the value
// it caches nothing of; the subject itself is on LVGL's list.
static_assert(sizeof(IntSubject) <= sizeof(lv_subject_t*) + sizeof(int32_t) + alignof(lv_subject_t*),
    "IntSubject should be a pointer and a value");
static_assert(sizeof(BoolSubject) <= sizeof(lv_subject_t*) + sizeof(bool) + alignof(lv_subject_t*),
    "BoolSubject should be a pointer and a value");
static_assert(sizeof(ColorSubject) <= sizeof(lv_subject_t*) + sizeof(lv_color_t) + alignof(lv_subject_t*),
    "ColorSubject should be a pointer and a value");

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
