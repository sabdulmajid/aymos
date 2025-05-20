# AymOS - A Lightweight Real-Time Operating System

AymOS is a sophisticated real-time operating system (RTOS) designed for embedded systems, specifically targeting ARM Cortex-M4 microcontrollers. Built from the ground up in C, it provides a robust foundation for real-time applications with a focus on efficiency, reliability, and precise timing control.

## Key Features

### Real-Time Task Management
- **Earliest Deadline First (EDF) Scheduling**: Implements a dynamic priority scheduling algorithm that ensures tasks meet their timing requirements
- **Priority Support**: Tasks have an explicit priority used to break deadline ties and can be changed at runtime
- **Task States**: Comprehensive task state management (READY, RUNNING, SLEEPING, DORMANT)
- **Deadline-Based Execution**: Tasks can be created with specific deadlines and time remaining parameters
- **Context Switching**: Efficient context switching mechanism using ARM's SVC and PendSV exceptions
- **Task Information**: Detailed task information retrieval and management capabilities

### Memory Management
- **Dynamic Memory Allocation**: Custom memory allocator with efficient block management
- **Memory Fragmentation Handling**: Built-in memory coalescing to prevent fragmentation
- **Memory Protection**: Task-specific memory ownership and access control
- **Stack Management**: Per-task stack allocation and management
- **Memory Statistics**: External fragmentation monitoring and reporting

### System Features
- **System Tick Timer**: Precise timing control with configurable system tick
- **Interrupt Handling**: Comprehensive interrupt management system
- **Task Synchronization**: Built-in mechanisms for task coordination
- **Error Handling**: Robust error detection and handling mechanisms
- **Debug Support**: Integrated debugging capabilities

## Technical Specifications

- **Architecture**: ARM Cortex-M4
- **Programming Language**: C
- **Memory Model**: Protected memory space with task isolation
- **Scheduling Algorithm**: EDF (Earliest Deadline First)
- **Interrupt Priority Levels**: Configurable interrupt priorities for system services

## Getting Started

### Prerequisites
- ARM Cortex-M4 compatible development board
- ARM GCC toolchain
- STM32CubeIDE or similar development environment

### Building the Project
```bash
# Clone the repository
git clone https://github.com/{username}/aymos.git

# Build the project
make
```

### Basic Usage
```c
// Initialize the kernel
osKernelInit();

// Create a task with deadline
TCB myTask;
myTask.ptask = myTaskFunction;
myTask.stack_size = STACK_SIZE;
osCreateDeadlineTask(deadline_ms, &myTask);

// Start the kernel
osKernelStart();
```

## API Reference

### Task Management
- `osKernelInit()`: Initialize the kernel
- `osKernelStart()`: Start the kernel and begin task execution
- `osCreateTask(TCB* task)`: Create a new task
- `osCreateDeadlineTask(int deadline, TCB* task)`: Create a task with specific deadline
- `osYield()`: Yield CPU to next task
- `osSleep(int timeInMs)`: Put current task to sleep
- `osTaskExit()`: Exit current task
- `osGetTID()`: Get current task ID
- `osSetPriority(uint8_t priority, task_t TID)`: Change task priority at runtime

### Memory Management
- `k_mem_init()`: Initialize memory management
- `k_mem_alloc(unsigned int size)`: Allocate memory
- `k_mem_dealloc(void* ptr)`: Deallocate memory
- `k_mem_count_extfrag(unsigned int size)`: Count external fragmentation


## Further Reading
See [docs/FUNCTIONALITY_OVERVIEW.md](docs/FUNCTIONALITY_OVERVIEW.md) for a more detailed description of how each module operates.
