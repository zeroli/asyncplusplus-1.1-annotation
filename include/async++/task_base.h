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

// Task states
enum class task_state: unsigned char {
	pending, // Task has not completed yet
	locked, // Task is locked (used by event_task to prevent double set)
	unwrapped, // Task is waiting for an unwrapped task to finish
	completed, // Task has finished execution and a result is available
	canceled // Task has been canceled and an exception is available
};

// Determine whether a task is in a final state
inline bool is_finished(task_state s)
{
	return s == task_state::completed || s == task_state::canceled;
}

// handy的virtual function table声称可以产生的代码比较小
// 所有的函数的第一个参数一定是task_base*指针类型，对应于virtual function的this指针
// Virtual function table used to allow dynamic dispatch for task objects.
// While this is very similar to what a compiler would generate with virtual
// functions, this scheme was found to result in significantly smaller
// generated code size.
struct task_base_vtable {
	// Destroy the function and result
	void (*destroy)(task_base*) LIBASYNC_NOEXCEPT;

	// Run the associated function
	void (*run)(task_base*) LIBASYNC_NOEXCEPT;

	// Cancel the task with an exception
	void (*cancel)(task_base*, std::exception_ptr&&) LIBASYNC_NOEXCEPT;

	// Schedule the task using its scheduler
	void (*schedule)(task_base* parent, task_ptr t);
};

// Type-generic base task object
struct task_base_deleter;

// 一个task也是cache line对齐的
// 继承于ref count类，这样就可以支持add_ref/remove_ref
// 从而支持以引用计数的方式被只能指针管理
struct LIBASYNC_CACHELINE_ALIGN task_base:
	public ref_count_base<task_base, task_base_deleter>
{
	// Task state
	std::atomic<task_state> state;

	// Whether get_task() was already called on an event_task
	bool event_task_got_task;

	// Vector of continuations
	continuation_vector continuations;

	// 没有定义任何virtual函数，自己提供虚函数表来实现所有操作都放在一个集合中
	// Virtual function table used for dynamic dispatch
	const task_base_vtable* vtable;

	// 类自己的new/delete操作符，调用到这里
	// 对齐到cache line
	// 有点疑惑，类定义已经申明了cache line对齐了，为啥还要实现特别的new/delete
	// 来分配对齐到cache line的内存呢？
	// Use aligned memory allocation
	static void* operator new(std::size_t size)
	{
		return aligned_alloc(size, LIBASYNC_CACHELINE_SIZE);
	}
	static void operator delete(void* ptr)
	{
		aligned_free(ptr);
	}

	// Initialize task state
	task_base()
		: state(task_state::pending) {}

	// Check whether the task is ready and include an acquire barrier if it is
	bool ready() const
	{
		return is_finished(state.load(std::memory_order_acquire));
	}

	// Run a single continuation
	template<typename Sched>
	void run_continuation(Sched& sched, task_ptr&& cont)
	{
		LIBASYNC_TRY {
			detail::schedule_task(sched, std::move(cont));
		} LIBASYNC_CATCH(...) {
			// This is suboptimal, but better than letting the exception leak
			cont->vtable->cancel(cont.get(), std::current_exception());
		}
	}

	// Run all of the task's continuations after it has completed or canceled.
	// The list of continuations is emptied and locked to prevent any further
	// continuations from being added.
	void run_continuations()
	{
		continuations.flush_and_lock([this](task_ptr t) {
			const task_base_vtable* vtable = t->vtable;
			vtable->schedule(this, std::move(t));
		});
	}

	// Add a continuation to this task
	template<typename Sched>
	void add_continuation(Sched& sched, task_ptr cont)
	{
		// Check for task completion
		task_state current_state = state.load(std::memory_order_relaxed);
		if (!is_finished(current_state)) {
			// Try to add the task to the continuation list. This can fail only
			// if the task has just finished, in which case we run it directly.
			if (continuations.try_add(std::move(cont)))
				return;
		}

		// Otherwise run the continuation directly
		std::atomic_thread_fence(std::memory_order_acquire);
		run_continuation(sched, std::move(cont));
	}

	// Finish the task after it has been executed and the result set
	void finish()
	{
		state.store(task_state::completed, std::memory_order_release);
		run_continuations();
	}

	// Wait for the task to finish executing
	task_state wait()
	{
		task_state s = state.load(std::memory_order_acquire);
		if (!is_finished(s)) {
			wait_for_task(this);
			s = state.load(std::memory_order_relaxed);
		}
		return s;
	}
};

// Deleter for task_ptr
struct task_base_deleter {
	static void do_delete(task_base* p)
	{
		// Go through the vtable to delete p with its proper type
		p->vtable->destroy(p);
	}
};

// Result type-specific task object
template<typename Result>
struct task_result_holder: public task_base {
	union {
		typename std::aligned_storage<sizeof(Result), std::alignment_of<Result>::value>::type result;
		std::aligned_storage<sizeof(std::exception_ptr), std::alignment_of<std::exception_ptr>::value>::type except;

		// Scheduler that should be used to schedule this task. The scheduler
		// type has been erased and is held by vtable->schedule.
		void* sched;
	};

	template<typename T>
	void set_result(T&& t)
	{
		new(&result) Result(std::forward<T>(t));
	}

	// Return a result using an lvalue or rvalue reference depending on the task
	// type. The task parameter is not used, it is just there for overload resolution.
	template<typename T>
	Result&& get_result(const task<T>&)
	{
		return std::move(*reinterpret_cast<Result*>(&result));
	}
	template<typename T>
	const Result& get_result(const shared_task<T>&)
	{
		return *reinterpret_cast<Result*>(&result);
	}

	// Destroy the result
	~task_result_holder()
	{
		// Result is only present if the task completed successfully
		if (state.load(std::memory_order_relaxed) == task_state::completed)
			reinterpret_cast<Result*>(&result)->~Result();
	}
};

// Specialization for references
template<typename Result>
struct task_result_holder<Result&>: public task_base {
	union {
		// Store as pointer internally
		Result* result;
		std::aligned_storage<sizeof(std::exception_ptr), std::alignment_of<std::exception_ptr>::value>::type except;
		void* sched;
	};

	void set_result(Result& obj)
	{
		result = std::addressof(obj);
	}

	template<typename T>
	Result& get_result(const task<T>&)
	{
		return *result;
	}
	template<typename T>
	Result& get_result(const shared_task<T>&)
	{
		return *result;
	}
};

// Specialization for void
template<>
struct task_result_holder<fake_void>: public task_base {
	union {
		std::aligned_storage<sizeof(std::exception_ptr), std::alignment_of<std::exception_ptr>::value>::type except;
		void* sched;
	};

	void set_result(fake_void) {}

	// Get the result as fake_void so that it can be passed to set_result and
	// continuations
	template<typename T>
	fake_void get_result(const task<T>&)
	{
		return fake_void();
	}
	template<typename T>
	fake_void get_result(const shared_task<T>&)
	{
		return fake_void();
	}
};

template<typename Result>
struct task_result: public task_result_holder<Result> {
	// Virtual function table for task_result
	static const task_base_vtable vtable_impl;
	task_result()
	{
		this->vtable = &vtable_impl;
	}

	// Destroy the exception
	~task_result()
	{
		// Exception is only present if the task was canceled
		if (this->state.load(std::memory_order_relaxed) == task_state::canceled)
			reinterpret_cast<std::exception_ptr*>(&this->except)->~exception_ptr();
	}

	// Cancel a task with the given exception
	void cancel_base(std::exception_ptr&& except)
	{
		set_exception(std::move(except));
		this->state.store(task_state::canceled, std::memory_order_release);
		this->run_continuations();
	}

	// Set the exception value of the task
	void set_exception(std::exception_ptr&& except)
	{
		new(&this->except) std::exception_ptr(std::move(except));
	}

	// Get the exception a task was canceled with
	std::exception_ptr& get_exception()
	{
		return *reinterpret_cast<std::exception_ptr*>(&this->except);
	}

	// Wait and throw the exception if the task was canceled
	void wait_and_throw()
	{
		if (this->wait() == task_state::canceled)
			LIBASYNC_RETHROW_EXCEPTION(get_exception());
	}

	// Delete the task using its proper type
	static void destroy(task_base* t) LIBASYNC_NOEXCEPT
	{
		delete static_cast<task_result<Result>*>(t);
	}
};
template<typename Result>
const task_base_vtable task_result<Result>::vtable_impl = {
	task_result<Result>::destroy, // destroy
	nullptr, // run
	nullptr, // cancel
	nullptr // schedule
};

// 这个类`func_base`的设计可以很好的学习下：
// 如何实现partial specialization从而进行优化：SFINAE + std::enable_if
// 如何针对大小为空的func进行优化
// Class to hold a function object, with empty base class optimization
template<typename Func, typename = void>
struct func_base {
	Func func;

	template<typename F>
	explicit func_base(F&& f)
		: func(std::forward<F>(f)) {}
	Func& get_func()
	{
		return func;
	}
};
template<typename Func>
struct func_base<Func, typename std::enable_if<std::is_empty<Func>::value>::type> {
	template<typename F>
	explicit func_base(F&& f)
	{
		new(this) Func(std::forward<F>(f));
	}
	~func_base()
	{
		get_func().~Func();
	}
	Func& get_func()
	{
		return *reinterpret_cast<Func*>(this);
	}
};

// 一个func holder类，内部开启一段内存区间
// 内存里面保存的对象，直到运行时特定时候才进行构造
// 这个类的设计可以学习下，开启内存空间采用aligned_storage
// 运行时手动构造，采用placement new
// 运行时手动析构，采用显示调用析构函数
// 同时这个类模板也支持局部特化来对空的数据类型进行优化
// Class to hold a function object and initialize/destroy it at any time
template<typename Func, typename = void>
struct func_holder {
	typename std::aligned_storage<sizeof(Func), std::alignment_of<Func>::value>::type func;

	Func& get_func()
	{
		return *reinterpret_cast<Func*>(&func);
	}
	template<typename... Args>
	void init_func(Args&&... args)
	{
		new(&func) Func(std::forward<Args>(args)...);
	}
	void destroy_func()
	{
		get_func().~Func();
	}
};
template<typename Func>
struct func_holder<Func, typename std::enable_if<std::is_empty<Func>::value>::type> {
	Func& get_func()
	{
		return *reinterpret_cast<Func*>(this);
	}
	template<typename... Args>
	void init_func(Args&&... args)
	{
		new(this) Func(std::forward<Args>(args)...);
	}
	void destroy_func()
	{
		get_func().~Func();
	}
};

// 这个task_func类继承于两个类
// 一个父类负责保存函数结果，一个父类保存函数本身
// 保存函数结果的类，继承于最上层的task_base基类，提供task的一些属性和接口
// 虽然是继承，但是并没有采用传统的虚函数的方式
// 自己手写virtual function table，针对类本身的，故用stack const静态数据成员
// Task object with an associated function object
// Using private inheritance so empty Func doesn't take up space
template<typename Sched, typename Func, typename Result>
struct task_func: public task_result<Result>, func_holder<Func> {
	// Virtual function table for task_func
	static const task_base_vtable vtable_impl;
	template<typename... Args>
	explicit task_func(Args&&... args)
	{
		this->vtable = &vtable_impl;
		this->init_func(std::forward<Args>(args)...);
	}

	// Run the stored function
	static void run(task_base* t) LIBASYNC_NOEXCEPT
	{
		LIBASYNC_TRY {
			// Dispatch to execution function
			// get_func()一般返回一个对root_exec_func的引用
			// 因为task_func保存的就是一个root_exec_func对象
			static_cast<task_func<Sched, Func, Result>*>(t)->get_func()(t);
		} LIBASYNC_CATCH(...) {
			cancel(t, std::current_exception());
		}
	}

	// Cancel the task
	static void cancel(task_base* t, std::exception_ptr&& except) LIBASYNC_NOEXCEPT
	{
		// Destroy the function object when canceling since it won't be
		// used anymore.
		static_cast<task_func<Sched, Func, Result>*>(t)->destroy_func();
		static_cast<task_func<Sched, Func, Result>*>(t)->cancel_base(std::move(except));
	}

	// Schedule a continuation task using its scheduler
	static void schedule(task_base* parent, task_ptr t)
	{
		void* sched = static_cast<task_func<Sched, Func, Result>*>(t.get())->sched;
		parent->run_continuation(*static_cast<Sched*>(sched), std::move(t));
	}

	// Free the function
	~task_func()
	{
		// If the task hasn't completed yet, destroy the function object. Note
		// that an unwrapped task has already destroyed its function object.
		if (this->state.load(std::memory_order_relaxed) == task_state::pending)
			this->destroy_func();
	}

	// Delete the task using its proper type
	static void destroy(task_base* t) LIBASYNC_NOEXCEPT
	{
		delete static_cast<task_func<Sched, Func, Result>*>(t);
	}
};
template<typename Sched, typename Func, typename Result>
const task_base_vtable task_func<Sched, Func, Result>::vtable_impl = {
	task_func<Sched, Func, Result>::destroy, // destroy
	task_func<Sched, Func, Result>::run, // run
	task_func<Sched, Func, Result>::cancel, // cancel
	task_func<Sched, Func, Result>::schedule // schedule
};

// Helper functions to access the internal_task member of a task object, which
// avoids us having to specify half of the functions in the detail namespace
// as friend. Also, internal_task is downcast to the appropriate task_result<>.
template<typename Task>
typename Task::internal_task_type* get_internal_task(const Task& t)
{
	return static_cast<typename Task::internal_task_type*>(t.internal_task.get());
}
template<typename Task>
void set_internal_task(Task& t, task_ptr p)
{
	t.internal_task = std::move(p);
}

// Common code for task unwrapping
template<typename Result, typename Child>
struct unwrapped_func {
	explicit unwrapped_func(task_ptr t)
		: parent_task(std::move(t)) {}
	void operator()(Child child_task) const
	{
		// Forward completion state and result to parent task
		task_result<Result>* parent = static_cast<task_result<Result>*>(parent_task.get());
		LIBASYNC_TRY {
			if (get_internal_task(child_task)->state.load(std::memory_order_relaxed) == task_state::completed) {
				parent->set_result(get_internal_task(child_task)->get_result(child_task));
				parent->finish();
			} else {
				// We don't call the generic cancel function here because
				// the function of the parent task has already been destroyed.
				parent->cancel_base(std::exception_ptr(get_internal_task(child_task)->get_exception()));
			}
		} LIBASYNC_CATCH(...) {
			// If the copy/move constructor of the result threw, propagate the exception
			parent->cancel_base(std::current_exception());
		}
	}
	task_ptr parent_task;
};
template<typename Sched, typename Result, typename Func, typename Child>
void unwrapped_finish(task_base* parent_base, Child child_task)
{
	// Destroy the parent task's function since it has been executed
	parent_base->state.store(task_state::unwrapped, std::memory_order_relaxed);
	static_cast<task_func<Sched, Func, Result>*>(parent_base)->destroy_func();

	// Set up a continuation on the child to set the result of the parent
	LIBASYNC_TRY {
		parent_base->add_ref();
		// `unwrapped_func`是一个函数对象，这里提供函数对象给then调用可以认为是一个task
		// 可是什么把child_task调用起来的呢？
		// 在执行父task最后返回child_task时，child_task就被放入到scheduler的队列中了
		// 同一个线程schedule task，将会在local queue中，取出并调用
		child_task.then(inline_scheduler(), unwrapped_func<Result, Child>(task_ptr(parent_base)));
	} LIBASYNC_CATCH(...) {
		// Use cancel_base here because the function object is already destroyed.
		static_cast<task_result<Result>*>(parent_base)->cancel_base(std::current_exception());
	}
}

// Execution functions for root tasks:
// - With and without task unwraping
template<typename Sched, typename Result, typename Func, bool Unwrap>
struct root_exec_func: private func_base<Func> {
	template<typename F>
	explicit root_exec_func(F&& f)
		: func_base<Func>(std::forward<F>(f)) {}

	// !!!!!!!!! task func执行入口  !!!!!!!!!
	void operator()(task_base* t)
	{
		// 触发调用，设置保存结果
		// `get_func`返回就是用户提供的可调用函数对象
		static_cast<task_result<Result>*>(t)->set_result(detail::invoke_fake_void(std::move(this->get_func())));
		// 销毁函数
		static_cast<task_func<Sched, root_exec_func, Result>*>(t)->destroy_func();
		// 标记结束并schedule continuation task
		t->finish();
	}
};
template<typename Sched, typename Result, typename Func>
struct root_exec_func<Sched, Result, Func, true>: private func_base<Func> {
	template<typename F>
	explicit root_exec_func(F&& f)
		: func_base<Func>(std::forward<F>(f)) {}
	void operator()(task_base* t)
	{
		// 注意这里的: `std::move(this->get_func())()`
		// 这里函数进行了调用，是父task进行了调用，返回值作为第二个参数传入`unwrapped_finish`
		// 第一个参数是父task，外围包裹task，需要等待子task完成返回后设置result给自己后才能返回
		unwrapped_finish<Sched, Result, root_exec_func>(t, std::move(this->get_func())());
	}
};

// Execution functions for continuation tasks:
// - With and without task unwraping
// - For void, value-based and task-based continuations
template<typename Sched, typename Parent, typename Result, typename Func, typename ValueCont, bool Unwrap>
struct continuation_exec_func: private func_base<Func> {
	template<typename F, typename P>
	continuation_exec_func(F&& f, P&& p)
		: func_base<Func>(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		static_cast<task_result<Result>*>(t)->set_result(detail::invoke_fake_void(std::move(this->get_func()), std::move(parent)));
		static_cast<task_func<Sched, continuation_exec_func, Result>*>(t)->destroy_func();
		t->finish();
	}
	Parent parent;
};
template<typename Sched, typename Parent, typename Result, typename Func>
struct continuation_exec_func<Sched, Parent, Result, Func, std::true_type, false>: private func_base<Func> {
	template<typename F, typename P>
	continuation_exec_func(F&& f, P&& p)
		: func_base<Func>(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		if (get_internal_task(parent)->state.load(std::memory_order_relaxed) == task_state::canceled)
			task_func<Sched, continuation_exec_func, Result>::cancel(t, std::exception_ptr(get_internal_task(parent)->get_exception()));
		else {
			static_cast<task_result<Result>*>(t)->set_result(detail::invoke_fake_void(std::move(this->get_func()), get_internal_task(parent)->get_result(parent)));
			static_cast<task_func<Sched, continuation_exec_func, Result>*>(t)->destroy_func();
			t->finish();
		}
	}
	Parent parent;
};
template<typename Sched, typename Parent, typename Result, typename Func>
struct continuation_exec_func<Sched, Parent, Result, Func, fake_void, false>: private func_base<Func> {
	template<typename F, typename P>
	continuation_exec_func(F&& f, P&& p)
		: func_base<Func>(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		if (get_internal_task(parent)->state.load(std::memory_order_relaxed) == task_state::canceled)
			task_func<Sched, continuation_exec_func, Result>::cancel(t, std::exception_ptr(get_internal_task(parent)->get_exception()));
		else {
			static_cast<task_result<Result>*>(t)->set_result(detail::invoke_fake_void(std::move(this->get_func()), fake_void()));
			static_cast<task_func<Sched, continuation_exec_func, Result>*>(t)->destroy_func();
			t->finish();
		}
	}
	Parent parent;
};
template<typename Sched, typename Parent, typename Result, typename Func>
struct continuation_exec_func<Sched, Parent, Result, Func, std::false_type, true>: private func_base<Func> {
	template<typename F, typename P>
	continuation_exec_func(F&& f, P&& p)
		: func_base<Func>(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		unwrapped_finish<Sched, Result, continuation_exec_func>(t, detail::invoke_fake_void(std::move(this->get_func()), std::move(parent)));
	}
	Parent parent;
};
template<typename Sched, typename Parent, typename Result, typename Func>
struct continuation_exec_func<Sched, Parent, Result, Func, std::true_type, true>: private func_base<Func> {
	template<typename F, typename P>
	continuation_exec_func(F&& f, P&& p)
		: func_base<Func>(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		if (get_internal_task(parent)->state.load(std::memory_order_relaxed) == task_state::canceled)
			task_func<Sched, continuation_exec_func, Result>::cancel(t, std::exception_ptr(get_internal_task(parent)->get_exception()));
		else
			unwrapped_finish<Sched, Result, continuation_exec_func>(t, detail::invoke_fake_void(std::move(this->get_func()), get_internal_task(parent)->get_result(parent)));
	}
	Parent parent;
};
template<typename Sched, typename Parent, typename Result, typename Func>
struct continuation_exec_func<Sched, Parent, Result, Func, fake_void, true>: private func_base<Func> {
	template<typename F, typename P>
	continuation_exec_func(F&& f, P&& p)
		: func_base<Func>(std::forward<F>(f)), parent(std::forward<P>(p)) {}
	void operator()(task_base* t)
	{
		if (get_internal_task(parent)->state.load(std::memory_order_relaxed) == task_state::canceled)
			task_func<Sched, continuation_exec_func, Result>::cancel(t, std::exception_ptr(get_internal_task(parent)->get_exception()));
		else
			unwrapped_finish<Sched, Result, continuation_exec_func>(t, detail::invoke_fake_void(std::move(this->get_func()), fake_void()));
	}
	Parent parent;
};

} // namespace detail
} // namespace async
