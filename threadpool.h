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
const int THREAD_MAX_IDLE_TIME = 10; // ��λs

// �̳߳�֧�ֵ�ģʽ
enum class PoolMode
{
    MODE_FIXED,  // �涨�������߳�
    MODE_CACHED, // �߳������ɶ�̬����
};



// �߳�����
class Thread
{
public:
    // �̺߳�����������
    using ThreadFunc = std::function<void(int)>;

    // �̹߳���
    Thread(ThreadFunc func) :func_(func),
        threadId_(generatedId_++)

    {
        std::cout << "�����̵߳Ĺ��캯��" << std::endl;
    }
    // �߳�����
    ~Thread() = default;
    void start()
    {
        // ����һ���߳���ִ��һ���̺߳���
        std::thread t(func_, threadId_); // C++11��˵ �̶߳���t ���߳�func_
        t.detach();                      // ���÷����߳�  pthread_detach ���óɷ����߳�
    }

    // ��ȡ�߳�ID
    int getId() const
    {
        return threadId_;
    }

private:
    ThreadFunc func_;
    static int generatedId_;
    int threadId_; // �����߳�ID
};
int Thread::generatedId_ = 0;

// �̳߳�����
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
        std::cout << "���캯��" << std::endl;
    }

    ~ThreadPool()
    {
        std::cout << "��������" << std::endl;

        isPoolRunning_ = false;

        // �ȴ��̳߳��������е��̷߳��� ������״̬�� ���� & ����ִ��������
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        notEmpty_.notify_all();
        exitCond_.wait(lock, [&]() -> bool
            { return threads_.size() == 0; });
    }

    // �����̳߳صĹ���ģʽ
    void setMode(PoolMode mode)
    {
        if (checkRunningState())
            return;
        this->poolMode_ = mode;
    }

    // ���ó�ʼ���߳�����
    void setInitThreadSize(int size)
    {
        if (checkRunningState())
            return;
        this->initThreadSize_ = size;
    }

    // ����task����������޵���ֵ
    void setTaskSizeMaxThreadHold(int threshhold)
    {
        if (checkRunningState())
            return;

        this->taskSizeMaxThreadHold_ = threshhold;
    }

    // �����̳߳�chchedģʽ���߳���ֵ

    void setThreadSizeThreshHold(int threshhold)
    {
        if (checkRunningState())
            return;

        // MODE_CACHED ģʽ�²�������
        if (poolMode_ == PoolMode::MODE_CACHED)
            this->threadSizeThreshHold_ = threshhold;
    }

    // ���̳߳��ύ����
    // ʹ�ÿɱ��ģ���� ����submitTask���Խ������������������������Ĳ���
    // pool.submit(sum1, 10, 20)
    // ����future<>
    template <typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
    {
        // ������񣬷����������
        using RType = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func),
           std::forward<Args>(args)...));
        std::future<RType> result = task->get_future();
        // ��ȡ��
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        // �̵߳�ͨѶ�� �ȴ���������п���
        if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]() -> bool
            { return taskQue_.size() < (size_t)taskSizeMaxThreadHold_; }))
        {
            // ��ʾ notFull_�ȴ�1s�� ������Ȼ��û������
            std::cerr << "task queue is full, submit task fail." << std::endl;
            auto task = std::make_shared<std::packaged_task<RType()>>(
                []()->RType {return RType();}
            );
            (*task)();
            return task->get_future();
        }

        // ����п��࣬������������������
        taskQue_.emplace([task]() {(*task)();});
        taskSize_++;

        // ��Ϊ�·������� ������п϶��ǲ����ˣ� ��noEmpty_�Ͻ���֪ͨ
        notEmpty_.notify_all();

        // cacheģ�� ������ȽϽ����� ������ С�����������Ҫ�����������ݺͿ����̵߳���������������
        if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
        {
            // ���� thread�̶߳����ʱ�򣬰��̺߳�������thread�̶߳���
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1)); // 14
            int threadId = ptr->getId();
            this->threads_.emplace(threadId, std::move(ptr));
            // �����߳�
            threads_[threadId]->start();
            // �޸ı���
            curThreadSize_++;
            idleThreadSize_++;

            std::cout << " create new thread! threadid:" << threadId << std::endl;
        }

        return result;
    }

    // �����̳߳�
    void start(int initThreadSize = std::thread::hardware_concurrency())
    {
        // ����pool����״̬
        this->isPoolRunning_ = true;
        // ��ʼ�̸߳���
        this->initThreadSize_ = initThreadSize;
        this->curThreadSize_ = initThreadSize;

        // �����̶߳���
        for (int i = 0; i < this->initThreadSize_; i++)
        {
            // ���� thread�̶߳����ʱ�򣬰��̺߳�������thread�̶߳���
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1)); // 14
            int threadId = ptr->getId();
            this->threads_.emplace(threadId, std::move(ptr));
        }

        // ���������߳�
        for (int i = 0; i < this->initThreadSize_; i++)
        {
            this->threads_[i]->start();
            idleThreadSize_++; // ��¼�����߳�
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    // �����̺߳���
    void threadFunc(int threadId)
    {
        auto lastTime = std::chrono::high_resolution_clock().now();

        for (;;) // ѭ��ȡ����
        {
            Task task;
            {

                // �Ȼ�ȡ��
                std::unique_lock<std::mutex> lock(taskQueMtx_); // �����������Զ��ͷ���

                std::cout << "�̣߳�" << std::this_thread::get_id() << "���Ի�ȡ����" << std::endl;
                while (taskQue_.size() == 0)
                {

                    //  �̳߳���������
                    if (!isPoolRunning_)
                    {
                        //  �̳߳�Ҫ�����ˣ� �����߳���Դ
                        std::cout << "threadId:" << std::this_thread::get_id() << "exit! \n" << std::endl;

                        threads_.erase(threadId);
                        exitCond_.notify_all();
                        return;
                    }

                    // cached ģ���£� �п����Ѿ������˺ܶ���̣߳����ǿ���ʱ�䳬��60s ��Ӧ�ðѶ�����̻߳���
                    // ��ǰʱ�� - ��һ���߳�ִ��ʱ�� > 60s

                    if (poolMode_ == PoolMode::MODE_CACHED)
                    {
                        // ����������ʱ������ 				// ÿһ���ӷ���һ�Σ� ��ô���� ��ʱ���أ� ����������ִ�з���
                        if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                        {
                            auto now = std::chrono::high_resolution_clock().now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            std::cout << "����ȴ�����" << std::endl;
                            if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
                            {
                                // ��ʼ���յ�ǰ�߳�
                                // ��¼�߳���������ر�����ֵ�޸�
                                // ���̶߳�����߳��б���ɾ��
                                std::cout << "���г�ʱ�� threadId:" << threadId << "exit! \n"
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
                        std::cout << "����ȴ�����" << std::endl;
                    }
                }

                std::cout << "�̣߳�" << std::this_thread::get_id() << "��ȡ����ɹ�" << std::endl;

                //  �߳�������  �������߳�����-1
                idleThreadSize_--;

                // �����������ȡһ���������
                task = taskQue_.front();
                taskQue_.pop(); // ɾ��֮�� �����������
                taskSize_--;

                // �����Ȼ����ʣ������ ����֪ͨ�����߳�ִ������
                if (taskQue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }

                // ȡ��һ������ ����֪ͨ, ֪ͨ���Լ����ύ��������
                notFull_.notify_all();
            }
            if (task != nullptr)
            {
                // task->run();  // ִ������ ������ķ���ֵsetVal������Result
                task();
            }

            // ��������
            idleThreadSize_++;

            // �����߳�ִ����������ʱ��
            lastTime = std::chrono::high_resolution_clock().now();
        }
    }

    // ���pool����״̬
    bool checkRunningState() const
    {
        return isPoolRunning_;
    }

private:
    // std::vector<std::unique_ptr<Thread>> threads_;  // �߳��б�
    std::unordered_map<int, std::unique_ptr<Thread>> threads_;

    size_t initThreadSize_;          // ��ʼ���߳�����
    std::atomic_int curThreadSize_;  // ��¼��ǰ�̳߳������̵߳�������
    std::atomic_int idleThreadSize_; // ��ǰ�߳��еĿ����߳�����
    int threadSizeThreshHold_;       // �߳�����������ֵ

    //std::queue<std::shared_ptr<Task>> taskQue_; // �������
    // Task���� -�� ��������
    using Task = std::function<void()>;
    std::queue<Task> taskQue_; // �������
    std::atomic_int taskSize_;                  // ���������
    int taskSizeMaxThreadHold_;                 // ��������������µ���ֵ

    std::mutex taskQueMtx_;            // ��֤������е��̰߳�ȫ
    std::condition_variable notFull_;  // ��ʾ������в��� �� submit����
    std::condition_variable notEmpty_; // ��ʾ������в��գ� run����
    std::condition_variable exitCond_; // ȫ���߳��˳�

    PoolMode poolMode_; // ��ǰ�̳߳صĹ���ģʽ

    std::atomic_bool isPoolRunning_; // ��ʾ��ǰ�̳߳ص�����״̬
};
#endif
