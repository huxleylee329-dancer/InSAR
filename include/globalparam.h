#pragma once
#include <vector>
#include <stdio.h>
#include <stdlib.h>

struct Heap //创建小顶堆以储存解缠邻接点序列
{
	int size;
	std::vector<int> x;
	std::vector<int> y;
	std::vector<double> queue;

public:
	Heap()         //初始化 
	{
		size = 0;
		// 初始预分配 1,000,000 空间，约占 16MB 内存，满足绝大多数解缠场景，且避免频繁重分配
		x.resize(1000000);
		y.resize(1000000);
		queue.resize(1000000);
	}
	~Heap()
	{
		// 析构时 std::vector 会自动释放内存，避免任何早期返回导致的内存泄漏
	}

	void shift_up(int i)  //上浮 
	{
		while (i > 1)
		{
			if (queue[i] < queue[i >> 1])
			{
				double temp = queue[i];
				queue[i] = queue[i >> 1];
				queue[i >> 1] = temp;
				int temp2 = x[i];
				x[i] = x[i >> 1];
				x[i >> 1] = temp2;
				temp2 = y[i];
				y[i] = y[i >> 1];
				y[i >> 1] = temp2;
			}
			i >>= 1;
		}
	}
	void shift_down(int i)   //下沉 
	{
		while ((i << 1) <= size)
		{
			int next = i << 1;
			if (next < size && queue[next + 1] < queue[next])
				next++;
			if (queue[i] > queue[next])
			{
				double temp = queue[i];
				queue[i] = queue[next];
				queue[next] = temp;
				int temp2 = x[i];
				x[i] = x[next];
				x[next] = temp2;
				temp2 = y[i];
				y[i] = y[next];
				y[next] = temp2;
				i = next;
			}
			else return;
		}
	}
	int push(double v, int map_x, int map_y)   //加入元素 
	{
		if (v < 0 ||
			map_x < 0 ||
			map_y < 0)
		{
			fprintf(stderr, "Heap::push(): input check failed!\n\n");
			return -1;
		}
		size++;
		// 动态检测是否需要扩容
		if (size >= (int)queue.size())
		{
			int new_cap = (int)queue.size() * 2;
			x.resize(new_cap);
			y.resize(new_cap);
			queue.resize(new_cap);
		}
		queue[size] = v;
		x[size] = map_x;
		y[size] = map_y;
		shift_up(size);
		return 0;
	}
	int pop()         //弹出操作 
	{
		if (size <= 0)
		{
			fprintf(stderr, "Heap::pop(): no data!\n\n");
			return -1;
		}
		double temp = queue[1];
		queue[1] = queue[size];
		queue[size] = temp;
		int temp2 = x[1];
		x[1] = x[size];
		x[size] = temp2;
		temp2 = y[1];
		y[1] = y[size];
		y[size] = temp2;
		size--;
		shift_down(1);
		return 0;
	}
	int top(int* x0, int* y0)			//输出根节点坐标
	{
		if (size <= 0)
		{
			fprintf(stderr, "Heap::top(): no data!\n\n");
			return -1;
		}
		*x0 = x[1];
		*y0 = y[1];
		return 1;
	}
	bool empty() const
	{
		return size == 0;
	}
};