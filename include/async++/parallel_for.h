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

// Internal implementation of parallel_for that only accepts a partitioner
// argument.
template<typename Sched, typename Partitioner, typename Func>
void internal_parallel_for(Sched& sched, Partitioner partitioner, const Func& func)
{
	// Split the partition, run inline if no more splits are possible
	auto subpart = partitioner.split();
	// 递归结束条件，不能再分的时候，到达最小单元
	// 在当前线程中直接执行task
	if (subpart.begin() == subpart.end()) {
		for (auto&& i: partitioner)
			func(std::forward<decltype(i)>(i));  // apply函数在range的一项上
		return;
	}
	/*
		假设有8个计算量，最小分割单元是2：
                      8 (T0)
			    /                 \
		(T1) 4                  4 (T0)
             /   \              /     \
	(T2) 2     2 (T1)   2 (T3)   2(T0)
	=> T0: 当前线程，也计算2个任务
	=> T1, T2, T3 => 线程池中的线程
	=> 注意这种策略需要thread pool支持从线程中提交task
	如果线程中的task太多，线程个数不多，提交的task没有空闲线程运行
	而当前线程又等待一般任务执行结束，就会陷入死锁
	因此thread pool需要支持：从线程提交的task优先进入本地线程的本地task queue中
	*/
	// Run the function over each half in parallel
	auto&& t = async::local_spawn(sched, [&sched, &subpart, &func] {
		detail::internal_parallel_for(sched, std::move(subpart), func);
	});
	detail::internal_parallel_for(sched, std::move(partitioner), func);
	// blocking等待前部分完成？
	t.get();
}

} // namespace detail

// 下面所有的parallel_for都指向这个函数
// Run a function for each element in a range
template<typename Sched, typename Range, typename Func>
void parallel_for(Sched& sched, Range&& range, const Func& func)
{
	// to_partitioner? 是如何实现的？
	detail::internal_parallel_for(sched, async::to_partitioner(std::forward<Range>(range)), func);
}

// Overload with default scheduler
template<typename Range, typename Func>
void parallel_for(Range&& range, const Func& func)
{
	async::parallel_for(::async::default_scheduler(), range, func);
}

// Overloads with std::initializer_list
template<typename Sched, typename T, typename Func>
void parallel_for(Sched& sched, std::initializer_list<T> range, const Func& func)
{
	async::parallel_for(sched, async::make_range(range.begin(), range.end()), func);
}
template<typename T, typename Func>
void parallel_for(std::initializer_list<T> range, const Func& func)
{
	async::parallel_for(async::make_range(range.begin(), range.end()), func);
}

} // namespace async
