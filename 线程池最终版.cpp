#include <iostream>
#include <future>
#include <thread>
using namespace std;


/*
	如何能让线程池提交任务更加方便
	1. pool.submitTask(sum1, 10, 20);
	2. 我们自己造了一个Result以及相关的类型，代码挺多
		C++11线程库 thread packaged_task（function函数对象） async


*/


int sum1(int a, int b)
{

	return a + b;
}

int sum2(int a, int b, int c)
{

	return a + b + c;
}


int main()
{
	thread t1(sum1, 10, 20);
	thread t1(sum2, 10, 20, 30);
	return 0;
}