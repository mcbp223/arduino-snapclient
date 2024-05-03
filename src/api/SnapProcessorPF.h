#pragma once
#include "api/SnapProcessor.h"
#include <stack>
#include <queue>
#include <mutex>

/**
 * @brief Processor for which the encoded output is buffered
 * PF = pool + fifo
 */
class SnapProcessorPF: public SnapProcessor {
 public:
  /// Default constructor
  SnapProcessorPF(size_t stackSize = RTOS_STACK_SIZE
      , UBaseType_t taskPriority = RTOS_TASK_PRIORITY
      , BaseType_t taskCore = tskNO_AFFINITY
      ) : SnapProcessor()
        , stack_size(stackSize)
        , task_priority(taskPriority)
        , task_core(taskCore)
  {
  }

  bool begin() override {
    if(!SnapProcessor::begin()) {
      ESP_LOGE(TAG, "SnapProcessor::begin() returned false");
      //?? return false;
    }

    psram_found = psramFound();

    task_allowed = (-1 <= task_core);

    if(task_allowed) {
      BaseType_t ret = xTaskCreateUniversal([](void* arg) {
        static_cast<SnapProcessorPF*>(arg)->taskFunc();
      }, "snap-decoder", stack_size, this, task_priority, &task_handle, task_core);

      if(ret != pdPASS) {
        ESP_LOGE(TAG, "cannot create Snap Decoder PF task");
        return false;
      }
    }

    return true;
  }

  void end(void) override {
    if(task_handle) {
      if(task_allowed) {
        task_allowed = false; // signal task to self terminate; not tested
        delay(10);
      }

      if(task_handle) {
        // task didn't terminate itself, force termination externally
        vTaskDelete(task_handle);
        task_handle = nullptr;
      }
    }

    // TODO free storage
    
    SnapProcessor::end();
  }

 protected:

  /// signals the need to delay receiving new chunks when buffers are full or memory is exhausted
  bool needsDelay() override {
    if(pool.empty() && fifo.empty())
      return false; // not started yet
    
    // TODO: recovery from memory errors, etc.
    if(!memory_exhausted) { // implementation not finalized
      if(psram_found) {
        ;
      } else {
        if(ESP.getFreeHeap() < 20*1024) {
          ESP_LOGE(TAG, "free heap exhausted, %u", ESP.getFreeHeap());
          memory_exhausted = true;
        } else if(ESP.getMaxAllocHeap() < 4096 - 16) {
          ESP_LOGE(TAG, "largest block exhausted, %u", ESP.getMaxAllocHeap());
          memory_exhausted = true;
        }
      }
    }

    if(true || memory_exhausted) {
      //
      if(!psram_found && allocated_messages > 120 && pool.empty())
        // will retry when buffers available in pool
        return true;
    }

    auto& tsync = p_snap_output->snapTimeSync();
    
    int64_t diff = last_fifo_push_micros - last_fifo_pop_micros;
    int64_t dset = 1000LL * (tsync.getStartDelay() - tsync.getProcessingLag());
    
    if(false && 0 == ::digitalRead(0)) {
      log_printf("snap fifo: %lld usec\n", diff);
    }
    
    //if(abs(diff) > std::numeric_limits<int>::max())
    //  return true;
    
    if(diff > dset)
      return true; // don't buffer more than instructed by server
    
    //if(diff > 1600LL * 1000) return false; // ESP32 is slow
    
    return false;
  }

  /// Buffers encoded chunks to achieve the delay specified in /etc/snapserver.conf.
  ///   The base class method plays them without delay.
  bool wireChunk(SnapMessageWireChunk &wire_chunk_message) override {
    message_type message;

    if(!pool.empty()) {
      guard_type guard(pool_mutex);
      message = std::move(pool.top());
      pool.pop();
    } else {
      // TODO: return false if no more memory
    
      message.id = allocated_messages++;
    }

    if(message.storage.size() < wire_chunk_message.size) {
      constexpr unsigned step = 240; // TODO: allocation policy

      while(crt_max_chunk_size < wire_chunk_message.size) {
        crt_max_chunk_size += step;
      }

      if(true && message.storage.size() > 0) {
        ESP_LOGI(TAG, "resizing chunk[%4u] from %u to %u -> %u"
          , message.id, message.storage.size(), wire_chunk_message.size, crt_max_chunk_size);
      }

      message.storage.resize(crt_max_chunk_size);
      if(message.storage.size() < crt_max_chunk_size) {
        ESP_LOGE(TAG, "failed to allocate %u bytes", crt_max_chunk_size);
        return false;
      }
    }

    message.timestamp = wire_chunk_message.timestamp;
    message.size = wire_chunk_message.size;
    message.payload = &message.storage[0];
    memcpy(message.payload, wire_chunk_message.payload, wire_chunk_message.size);

    last_fifo_push_micros = message.timestamp.toMicros();

    fifo_mutex.lock();
    fifo.push(std::move(message));
    fifo_mutex.unlock();

    return true;
  }

  /// Extracts one chunk from the FIFO and passes it to the decoder via base::wireChunk(...).
  bool dequeue() {
    if(fifo.empty())
      return false;
    
    message_type fifo_chunk_message;

    fifo_mutex.lock();
    fifo_chunk_message = std::move(fifo.front());
    fifo.pop();
    fifo_mutex.unlock();

    last_fifo_pop_micros = fifo_chunk_message.timestamp.toMicros();

    // in order to release the fifo buffer back to the pool early,
    //   we copy it to another buffer that will be active during the decoding process; prefferable in internal RAM
    //   (not sure if necessary, test with slower MCU and bigger overhead = smaller chunk_ms in /etc/snapserver.conf)
    SnapMessageWireChunk wire_chunk_message;
    wire_chunk_message.size = fifo_chunk_message.size;
    wire_chunk_message.timestamp = fifo_chunk_message.timestamp;
    fifo_out_buffer.resize(crt_max_chunk_size);
    wire_chunk_message.payload = &fifo_out_buffer[0];
    memcpy(wire_chunk_message.payload, fifo_chunk_message.payload, fifo_chunk_message.size);

    pool_mutex.lock();
    pool.push(std::move(fifo_chunk_message));
    pool_mutex.unlock();

    // make sure we're not calling self::wireChunk(..) in a infinite loop
    SnapProcessor::wireChunk(wire_chunk_message); // blocking;
    
    return true;
  }

  /// override method; used here for processing chunks after FIFO delay, when not done in a separate thread.
  void processExt() override {
    if(!task_handle) {
      dequeue();
    }

    delay(1);
  }

  /// thread function, processes chunks after FIFO delay.
  void taskFunc() {
    int delcnt = 0;
    while(task_allowed) {
      dequeue();

      if(1 < ++delcnt) {
        delay(1);
        delcnt = 0;
      }
    }

    vTaskDelete(task_handle);
    task_handle = nullptr;
  }

 private:

//using storage_type = std::vector<char>; // needs template parameter for PSRAM allocator
  using storage_type = Vector<char>; // default allocator tries PSRAM first

  /// structure encapsulating the timestamp together with the chunk it refers to
  struct message_type: SnapMessageWireChunk {
    storage_type storage;
    unsigned id;

    message_type() = default;
    
    message_type(const message_type&) = delete;
    message_type& operator =(const message_type&) = delete; // making sure it doesn't copy on vector resize

    message_type(message_type&&) = default;
    message_type& operator =(message_type&&) = default;
  };

  using mutex_type = std::mutex;
  using guard_type = std::lock_guard<mutex_type>;

  using fifo_type = std::queue<message_type>;
  using pool_type = std::stack<message_type>; // used as LIFO, hoping the most recent buffer is in cache
  
  fifo_type fifo;
  pool_type pool;

  mutex_type fifo_mutex;
  mutex_type pool_mutex;

  //storage_type fifo_out_buffer;
  std::vector<char> fifo_out_buffer; // prefferable array allocated in internal RAM

  TaskHandle_t task_handle = nullptr; // maybe needs volatile; check if library class can be used

  size_t stack_size = RTOS_STACK_SIZE;
  UBaseType_t task_priority = RTOS_TASK_PRIORITY;
   BaseType_t task_core = -1; // -1 = don't care

  volatile bool task_allowed = false;

  int64_t last_fifo_push_micros = 0;
  int64_t last_fifo_pop_micros = 0;
  unsigned crt_max_chunk_size = 0;
  unsigned allocated_messages = 0;
  bool memory_exhausted = false;
  bool psram_found = false;
};
