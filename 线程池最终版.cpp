#include <iostream>
#include <future>
#include <thread>
using namespace std;


/*
	��������̳߳��ύ������ӷ���
	1. pool.submitTask(sum1, 10, 20);
	2. �����Լ�����һ��Result�Լ���ص����ͣ�����ͦ��
		C++11�߳̿� thread packaged_task��function�������� async


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