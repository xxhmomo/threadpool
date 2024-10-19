#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <future>

const int TASK_MAX_THRESHHOLD = 2; //INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 100;
const int THREAD_MAX_IDLE_TIME = 10; // 单位s

// 线程池支持的模式
enum class PoolMode
{
    MODE_FIXED,  // 规定熟练的线程
    MODE_CACHED, // 线程数量可动态增长
};



// 线程类型
class Thread
{
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(int)>;

    // 线程构造
    Thread(ThreadFunc func) :func_(func),
        threadId_(generatedId_++)

    {
        std::cout << "单独线程的构造函数" << std::endl;
    }
    // 线程析构
    ~Thread() = default;
    void start()
    {
        // 创建一个线程来执行一个线程函数
        std::thread t(func_, threadId_); // C++11来说 线程对象t 和线程func_
        t.detach();                      // 设置分离线程  pthread_detach 设置成分离线程
    }

    // 获取线程ID
    int getId() const
    {
        return threadId_;
    }

private:
    ThreadFunc func_;
    static int generatedId_;
    int threadId_; // 保存线程ID
};
int Thread::generatedId_ = 0;

// 线程池类型
class ThreadPool
{
public:
    ThreadPool() : initThreadSize_(0),
        taskSize_(0),
        taskSizeMaxThreadHold_(TASK_MAX_THRESHHOLD),
        poolMode_(PoolMode::MODE_FIXED),
        idleThreadSize_(0),
        threadSizeThreshHold_(THREAD_MAX_THRESHHOLD),
        curThreadSize_(0),
        isPoolRunning_(false)
    {
        std::cout << "构造函数" << std::endl;
    }

    ~ThreadPool()
    {
        std::cout << "析构函数" << std::endl;

        isPoolRunning_ = false;

        // 等待线程池里面所有的线程返回 有两种状态： 阻塞 & 正在执行任务中
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        notEmpty_.notify_all();
        exitCond_.wait(lock, [&]() -> bool
            { return threads_.size() == 0; });
    }

    // 设置线程池的工作模式
    void setMode(PoolMode mode)
    {
        if (checkRunningState())
            return;
        this->poolMode_ = mode;
    }

    // 设置初始的线程数量
    void setInitThreadSize(int size)
    {
        if (checkRunningState())
            return;
        this->initThreadSize_ = size;
    }

    // 设置task任务队列上限的阈值
    void setTaskSizeMaxThreadHold(int threshhold)
    {
        if (checkRunningState())
            return;

        this->taskSizeMaxThreadHold_ = threshhold;
    }

    // 设置线程池chched模式下线程阈值

    void setThreadSizeThreshHold(int threshhold)
    {
        if (checkRunningState())
            return;

        // MODE_CACHED 模式下才能设置
        if (poolMode_ == PoolMode::MODE_CACHED)
            this->threadSizeThreshHold_ = threshhold;
    }

    // 给线程池提交任务
    // 使用可变参模板编程 ，让submitTask可以姐搜任意任务函数和任意数量的参数
    // pool.submit(sum1, 10, 20)
    // 返回future<>
    template <typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
    {
        // 打包任务，放入任务队列
        using RType = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func),
           std::forward<Args>(args)...));
        std::future<RType> result = task->get_future();
        // 获取锁
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        // 线程的通讯， 等待任务队列有空余
        if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]() -> bool
            { return taskQue_.size() < (size_t)taskSizeMaxThreadHold_; }))
        {
            // 表示 notFull_等待1s， 条件依然还没有满足
            std::cerr << "task queue is full, submit task fail." << std::endl;
            auto task = std::make_shared<std::packaged_task<RType()>>(
                []()->RType {return RType();}
            );
            (*task)();
            return task->get_future();
        }

        // 如果有空余，把任务放入任务队列中
        taskQue_.emplace([task]() {(*task)();});
        taskSize_++;

        // 因为新放了任务， 任务队列肯定是不空了， 在noEmpty_上进行通知
        notEmpty_.notify_all();

        // cache模型 任务处理比较紧急， 场景： 小而快的任务，需要根据任务数据和空闲线程的数量，进行扩容
        if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
        {
            // 创建 thread线程对象的时候，把线程函数给到thread线程对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1)); // 14
            int threadId = ptr->getId();
            this->threads_.emplace(threadId, std::move(ptr));
            // 启动线程
            threads_[threadId]->start();
            // 修改变量
            curThreadSize_++;
            idleThreadSize_++;

            std::cout << " create new thread! threadid:" << threadId << std::endl;
        }

        return result;
    }

    // 开启线程池
    void start(int initThreadSize = std::thread::hardware_concurrency())
    {
        // 设置pool运行状态
        this->isPoolRunning_ = true;
        // 初始线程个数
        this->initThreadSize_ = initThreadSize;
        this->curThreadSize_ = initThreadSize;

        // 创建线程对象
        for (int i = 0; i < this->initThreadSize_; i++)
        {
            // 创建 thread线程对象的时候，把线程函数给到thread线程对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1)); // 14
            int threadId = ptr->getId();
            this->threads_.emplace(threadId, std::move(ptr));
        }

        // 启动所有线程
        for (int i = 0; i < this->initThreadSize_; i++)
        {
            this->threads_[i]->start();
            idleThreadSize_++; // 记录空闲线程
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    // 定义线程函数
    void threadFunc(int threadId)
    {
        auto lastTime = std::chrono::high_resolution_clock().now();

        for (;;) // 循环取任务
        {
            Task task;
            {

                // 先获取锁
                std::unique_lock<std::mutex> lock(taskQueMtx_); // 出了作用域自动释放锁

                std::cout << "线程：" << std::this_thread::get_id() << "尝试获取任务" << std::endl;
                while (taskQue_.size() == 0)
                {

                    //  线程池正在析构
                    if (!isPoolRunning_)
                    {
                        //  线程池要结束了， 回收线程资源
                        std::cout << "threadId:" << std::this_thread::get_id() << "exit! \n" << std::endl;

                        threads_.erase(threadId);
                        exitCond_.notify_all();
                        return;
                    }

                    // cached 模型下， 有可能已经创建了很多的线程，但是空闲时间超过60s ，应该把多余的线程回收
                    // 当前时间 - 上一次线程执行时间 > 60s

                    if (poolMode_ == PoolMode::MODE_CACHED)
                    {
                        // 条件变量超时返回了 				// 每一秒钟返回一次， 怎么区别： 超时返回？ 还是有任务执行返回
                        if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                        {
                            auto now = std::chrono::high_resolution_clock().now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            std::cout << "进入等待队列" << std::endl;
                            if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
                            {
                                // 开始回收当前线程
                                // 记录线程数量的相关变量的值修改
                                // 把线程对象从线程列表中删除
                                std::cout << "空闲超时： threadId:" << threadId << "exit! \n"
                                    << std::endl;
                                threads_.erase(threadId);
                                curThreadSize_--;
                                idleThreadSize_--;

                                return;
                            }
                        }
                    }
                    else
                    {
                        notEmpty_.wait(lock);
                        std::cout << "进入等待队列" << std::endl;
                    }
                }

                std::cout << "线程：" << std::this_thread::get_id() << "获取任务成功" << std::endl;

                //  线程起来了  ，空闲线程数量-1
                idleThreadSize_--;

                // 从任务队列中取一个任务出来
                task = taskQue_.front();
                taskQue_.pop(); // 删除之后， 对象就析构了
                taskSize_--;

                // 如果依然存在剩余任务， 继续通知其他线程执行任务
                if (taskQue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }

                // 取出一个任务， 进行通知, 通知可以继续提交生产任务
                notFull_.notify_all();
            }
            if (task != nullptr)
            {
                // task->run();  // 执行任务， 把任务的返回值setVal方法到Result
                task();
            }

            // 处理完了
            idleThreadSize_++;

            // 更新线程执行完成任务的时间
            lastTime = std::chrono::high_resolution_clock().now();
        }
    }

    // 检查pool运行状态
    bool checkRunningState() const
    {
        return isPoolRunning_;
    }

private:
    // std::vector<std::unique_ptr<Thread>> threads_;  // 线程列表
    std::unordered_map<int, std::unique_ptr<Thread>> threads_;

    size_t initThreadSize_;          // 初始的线程数量
    std::atomic_int curThreadSize_;  // 记录当前线程池里面线程的总数量
    std::atomic_int idleThreadSize_; // 当前线程中的空闲线程数量
    int threadSizeThreshHold_;       // 线程数量上限阈值

    //std::queue<std::shared_ptr<Task>> taskQue_; // 任务队列
    // Task任务 -》 函数对象
    using Task = std::function<void()>;
    std::queue<Task> taskQue_; // 任务队列
    std::atomic_int taskSize_;                  // 任务的数量
    int taskSizeMaxThreadHold_;                 // 任务队列数量上下的阈值

    std::mutex taskQueMtx_;            // 保证任务队列的线程安全
    std::condition_variable notFull_;  // 表示任务队列不满 ， submit任务
    std::condition_variable notEmpty_; // 表示任务队列不空， run任务
    std::condition_variable exitCond_; // 全部线程退出

    PoolMode poolMode_; // 当前线程池的工作模式

    std::atomic_bool isPoolRunning_; // 表示当前线程池的启动状态
};
#endif
