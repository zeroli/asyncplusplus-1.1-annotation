// Copyright (c) 2015 Amanieu d'Antras
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ASYNCXX_H_
# error "Do not include this header directly, include <async++.h> instead."
#endif

namespace async {
namespace detail {

// 下面fake-void很有意思
// 如何表示空类型？空类
// 一个空类对应void类型，利用类模板的特化和函数重载，来实现两者间之间相互转换
// Pseudo-void type: it takes up no space but can be moved and copied
struct fake_void {};
template<typename T>
struct void_to_fake_void {
	typedef T type;
};
template<>
struct void_to_fake_void<void> {
	typedef fake_void type;
};
template<typename T>
T fake_void_to_void(T&& x)
{
	return std::forward<T>(x);
}
inline void fake_void_to_void(fake_void) {}

// Check if type is a task type, used to detect task unwraping
template<typename T>
struct is_task: public std::false_type {};
template<typename T>
struct is_task<task<T>>: public std::true_type {};
template<typename T>
struct is_task<const task<T>>: public std::true_type {};
template<typename T>
struct is_task<shared_task<T>>: public std::true_type {};
template<typename T>
struct is_task<const shared_task<T>>: public std::true_type {};

// 去除一个task<T>模板类的task包裹器，返回它包裹的类型
// 仍然利用类模板的偏特化来实现
// Extract the result type of a task if T is a task, otherwise just return T
template<typename T>
struct remove_task {
	typedef T type;
};
template<typename T>
struct remove_task<task<T>> {
	typedef T type;
};
template<typename T>
struct remove_task<const task<T>> {
	typedef T type;
};
template<typename T>
struct remove_task<shared_task<T>> {
	typedef T type;
};
template<typename T>
struct remove_task<const shared_task<T>> {
	typedef T type;
};

// 如何判断一个类型是可以被调用？？
// 函数，提供operator()的函数对象，lambda表示
// Check if a type is callable with the given arguments
typedef char one[1];
typedef char two[2];
template<typename Func, typename... Args, typename = decltype(std::declval<Func>()(std::declval<Args>()...))>
two& is_callable_helper(int);
template<typename Func, typename... Args>
one& is_callable_helper(...);
template<typename T>
struct is_callable;
template<typename Func, typename... Args>
struct is_callable<Func(Args...)>: public std::integral_constant<bool, sizeof(is_callable_helper<Func, Args...>(0)) - 1> {};

// `invoke_fake_void`提供2类调用方式
// 一个是无参数调用，一个是带参数调用
// 两者都提供返回值，一种是void返回，一种具体的返回值
// 返回值用一个模板类来包装两个不同的返回类型: void => fake_void, result => result
// void用一个特别的空类来表示
// 下面代码实现用到了SFINAE + std::enable_if技巧来支持函数重载
// std::enable_if是描述在模板参数中
// Wrapper to run a function object with an optional parameter:
// - void returns are turned into fake_void
// - fake_void parameter will invoke the function with no arguments

// 这个写法有点啰嗦，为啥不直接将enable_if写在返回值中，譬如下面：
/*
	>>> 推导返回值不是void类型，直接返回那个类型
	template <typename Func>
	std::enable_if<!std::is_void<decltype(std::declval<Func>()())>::value, decltype(std::declval<Func>()())>
	invoke_fake_void(Func&& f);
	>>> 推导返回值是void类型，返回fake_void
	template <typename Func>
	std::enable_if<std::is_void<decltype(std::declval<Func>()())>::value, fake_void>
	invoke_fake_void(Func&& f);
*/
template<typename Func, typename = typename std::enable_if<!std::is_void<decltype(std::declval<Func>()())>::value>::type>
decltype(std::declval<Func>()()) invoke_fake_void(Func&& f)
{
	return std::forward<Func>(f)();
}
template<typename Func, typename = typename std::enable_if<std::is_void<decltype(std::declval<Func>()())>::value>::type>
fake_void invoke_fake_void(Func&& f)
{
	// !!!!!!用户可调用对象被调用的地方!!!!!!!
	std::forward<Func>(f)();
	return fake_void();
}
// 带参数的参数调用，将函数和参数捕获到lambda表达式中，从而转调用无参数的函数版本
template<typename Func, typename Param>
typename void_to_fake_void<decltype(std::declval<Func>()(std::declval<Param>()))>::type
invoke_fake_void(Func&& f, Param&& p)
{
	return detail::invoke_fake_void([&f, &p] {return std::forward<Func>(f)(std::forward<Param>(p));});
}
template<typename Func>
typename void_to_fake_void<decltype(std::declval<Func>()())>::type
invoke_fake_void(Func&& f, fake_void)
{
	return detail::invoke_fake_void(std::forward<Func>(f));
}

// Various properties of a continuation function
template<typename Func, typename Parent, typename = decltype(std::declval<Func>()())>
fake_void is_value_cont_helper(const Parent&, int, int);
template<typename Func, typename Parent, typename = decltype(std::declval<Func>()(std::declval<Parent>().get()))>
std::true_type is_value_cont_helper(const Parent&, int, int);
template<typename Func, typename = decltype(std::declval<Func>()())>
std::true_type is_value_cont_helper(const task<void>&, int, int);
template<typename Func, typename = decltype(std::declval<Func>()())>
std::true_type is_value_cont_helper(const shared_task<void>&, int, int);
template<typename Func, typename Parent, typename = decltype(std::declval<Func>()(std::declval<Parent>()))>
std::false_type is_value_cont_helper(const Parent&, int, ...);
template<typename Func, typename Parent>
void is_value_cont_helper(const Parent&, ...);
template<typename Parent, typename Func>
struct continuation_traits {
	typedef typename std::decay<Func>::type decay_func;
	typedef decltype(detail::is_value_cont_helper<decay_func>(std::declval<Parent>(), 0, 0)) is_value_cont;
	static_assert(!std::is_void<is_value_cont>::value, "Parameter type for continuation function is invalid for parent task type");
	typedef typename std::conditional<std::is_same<is_value_cont, fake_void>::value, fake_void, typename std::conditional<std::is_same<is_value_cont, std::true_type>::value, typename void_to_fake_void<decltype(std::declval<Parent>().get())>::type, Parent>::type>::type param_type;
	typedef decltype(detail::fake_void_to_void(detail::invoke_fake_void(std::declval<decay_func>(), std::declval<param_type>()))) result_type;
	typedef task<typename remove_task<result_type>::type> task_type;
};

} // namespace detail
} // namespace async
