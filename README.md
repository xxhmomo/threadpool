## 基于可变参模板实现的线程池
### 工具平台
vs2019开发， centos7编译so库，gdb调试分析定位死锁问题
### 项目描述
基于可变参模板编程和引用折叠原理，实现线程池submitTask接口，支持任意任务函数和任意参数的传递
使用future类型定制submitTask提高任务的返回
使用map和quque容器管理线程对象和任务
使用条件变量condition_variable和互斥锁mutex实现任务提高线程和任务执行线程间的通讯机制
支持fixed和cached模式的线程池定制
使用多态，模板，智能指针，mutex， condition_variable模拟实现Future类

