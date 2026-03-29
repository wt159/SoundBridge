#pragma once
#include "Optional.hpp"
#include <functional>
template <typename T> struct Lazy {
    template <typename Func, typename... Args> Lazy(Func f, Args... args)
    {
        m_func = [f, args...]() mutable { return f(args...); };
    }

    T &Value()
    {
        if (!m_value.isInit()) {
            m_value = m_func();
        }

        return *m_value;
    }

    bool IsValueCreated() const { return m_value.isInit(); }

private:
    std::function<T()> m_func;
    Optional<T> m_value;
};

template <class Func, typename... Args>
Lazy<typename std::result_of<Func(Args...)>::type> lazy(Func &&fun, Args &&...args)
{
    return Lazy<typename std::result_of<Func(Args...)>::type>(std::forward<Func>(fun),
                                                              std::forward<Args>(args)...);
}
