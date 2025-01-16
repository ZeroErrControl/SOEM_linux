/* 
 * This program is an EtherCAT master implementation that initializes and configures EtherCAT slaves,
 * manages their states, and handles real-time data exchange. It includes functions for setting up 
 * PDO mappings, synchronizing time with the distributed clock, and controlling servomotors in 
 * various operational modes. The program also features multi-threading for real-time processing 
 * and monitoring of the EtherCAT network.
 */
//#include <QCoreApplication>


#include <stdio.h>
#include <string.h>
#include "ethercat.h"
#include <iostream>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>

#include <sys/time.h>
#include <pthread.h>
#include <math.h>

#include <chrono>
#include <ctime>

#include <iostream>
#include <cstdint>

#include <sched.h>


// Global variables for EtherCAT communication
char IOmap[4096]; // I/O mapping for EtherCAT
int expectedWKC; // Expected Work Counter
boolean needlf; // Flag to indicate if a line feed is needed
volatile int wkc; // Work Counter (volatile to ensure it is updated correctly in multi-threaded context)
boolean inOP; // Flag to indicate if the system is in operational state
uint8 currentgroup = 0; // Current group for EtherCAT communication
int dorun = 0; // Flag to indicate if the thread should run
bool start_ecatthread_thread; // Flag to start the EtherCAT thread
int ctime_thread; // Cycle time for the EtherCAT thread

int64 toff, gl_delta; // Time offset and global delta for synchronization

// Function prototypes for EtherCAT thread functions
OSAL_THREAD_FUNC ecatcheck(void *ptr); // Function to check the state of EtherCAT slaves
OSAL_THREAD_FUNC ecatthread(void *ptr); // Real-time EtherCAT thread function

// Thread handles for the EtherCAT threads
OSAL_THREAD_HANDLE thread1; // Handle for the EtherCAT check thread
OSAL_THREAD_HANDLE thread2; // Handle for the real-time EtherCAT thread

// Function to synchronize time with the EtherCAT distributed clock
void ec_sync(int64 reftime, int64 cycletime, int64 *offsettime);
// Function to add nanoseconds to a timespec structure
void add_timespec(struct timespec *ts, int64 addtime);

// Define constants for stack size and timing
#define stack64k (64 * 1024) // Stack size for threads
#define NSEC_PER_SEC 1000000000   // Number of nanoseconds in one second
#define EC_TIMEOUTMON 5000        // Timeout for monitoring in microseconds

// Conversion units from the servomotor
float Cnt_to_deg = 0.000686645; // Conversion factor from counts to degrees
int8_t SLAVE_ID; // Slave ID for EtherCAT communication

// 在文件开始处添加卡尔曼滤波器类
class KalmanFilter {
private:
    float Q;      // 过程噪声协方差
    float R;      // 测量噪声协方差
    float P;      // 估计误差协方差
    float K;      // 卡尔曼增益
    float x_est;  // 当前估计值

public:
    KalmanFilter(float Q = 0.1, float R = 1.0) {
        this->Q = Q;
        this->R = R;
        this->P = 1.0;
        this->K = 0.0;
        this->x_est = 0.0;
    }

    float update(float measurement) {
        // 预测
        P = P + Q;

        // 更新
        K = P / (P + R);
        x_est = x_est + K * (measurement - x_est);
        P = (1 - K) * P;

        return x_est;
    }

    void reset() {
        P = 1.0;
        x_est = 0.0;
    }

    void setParameters(float new_Q, float new_R) {
        Q = new_Q;
        R = new_R;
    }
};

//##################################################################################################
// Function: Set the CPU affinity for a thread
void set_thread_affinity(pthread_t thread, int cpu_core) {
    cpu_set_t cpuset; // CPU set to specify which CPUs the thread can run on
    CPU_ZERO(&cpuset); // Clear the CPU set
    CPU_SET(cpu_core, &cpuset); // Add the specified CPU core to the set

    // Set the thread's CPU affinity
    int result = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        printf("Unable to set CPU affinity for thread %d\n", cpu_core); // Error message if setting fails
    } else {
        printf("Thread successfully bound to CPU %d\n", cpu_core); // Confirmation message if successful
    }
}

//##################################################################################################
// Function prototype for the EtherCAT test function
int erob_test();

uint16_t data_R;

int erob_test() {
    int rdl; // Variable to hold read data length
    SLAVE_ID = 1; // Set the slave ID to 1
    int i, j, oloop, iloop, chk; // Loop control variables
    uint32_t max_velocity = 50000;

    // 阻抗控制参数
    float M = 0.5;    // 虚拟质量
    float B = 50.0;   // 阻尼系数
    float K = 100.0;  // 刚度
    float dt = 0.001; // 控制周期 (1ms)
    
    // 阻抗控制变量
    float current_velocity = 0.0;
    float desired_force = 0.0;  // 期望交互力
    float force_error = 0.0;
    float velocity_cmd = 0.0;

    // 1. Call ec_config_init() to move from INIT to PRE-OP state.
    printf("__________STEP 1___________________\n");
    // Initialize EtherCAT master on the specified network interface
    if (ec_init("enp6s0") <= 0) {
        printf("Error: Could not initialize EtherCAT master!\n");
        printf("No socket connection on Ethernet port. Execute as root.\n");
        printf("___________________________________________\n");
        return -1; // Return error if initialization fails
    }
    printf("EtherCAT master initialized successfully.\n");
    printf("___________________________________________\n");

    // Search for EtherCAT slaves on the network
    if (ec_config_init(FALSE) <= 0) {
        printf("Error: Cannot find EtherCAT slaves!\n");
        printf("___________________________________________\n");
        ec_close(); // Close the EtherCAT connection
        return -1; // Return error if no slaves are found
    }
    printf("%d slaves found and configured.\n", ec_slavecount); // Print the number of slaves found
    printf("___________________________________________\n");

    // 2. Change to pre-operational state to configure the PDO registers
    printf("__________STEP 2___________________\n");

    // Check if the slave is ready to map
    ec_readstate(); // Read the state of the slaves

    for(int i = 1; i <= ec_slavecount; i++) { // Loop through each slave
        if(ec_slave[i].state != EC_STATE_PRE_OP) { // If the slave is not in PRE-OP state
            // Print the current state and status code of the slave
            printf("Slave %d State=0x%2.2x StatusCode=0x%4.4x : %s\n",
                   i, ec_slave[i].state, ec_slave[i].ALstatuscode, ec_ALstatuscode2string(ec_slave[i].ALstatuscode));
            printf("\nRequest init state for slave %d\n", i); // Request to change the state to INIT
            ec_slave[i].state = EC_STATE_INIT; // Set the slave state to INIT
            printf("___________________________________________\n");
        } else { // If the slave is in PRE-OP state
            ec_slave[0].state = EC_STATE_PRE_OP; // Set the first slave to PRE-OP state
            /* Request EC_STATE_PRE_OP state for all slaves */
            ec_writestate(0); // Write the state change to the slave
            /* Wait for all slaves to reach the PRE-OP state */
            if ((ec_statecheck(0, EC_STATE_PRE_OP,  3 * EC_TIMEOUTSTATE)) == EC_STATE_PRE_OP) {
                printf("State changed to EC_STATE_PRE_OP: %d \n", EC_STATE_PRE_OP);
                printf("___________________________________________\n");
            } else {
                printf("State EC_STATE_PRE_OP cannot be changed in step 2\n");
                return -1; // Return error if state change fails
            }
        }
    }

//##################################################################################################
    //3.- Map RXPOD
    printf("__________STEP 3___________________\n");

    // Clear RXPDO mapping
    int retval = 0; // Variable to hold the return value of SDO write operations
    uint16 map_1c12; // Variable to hold the mapping for PDO
    uint8 zero_map = 0; // Variable to clear the PDO mapping
    uint32 map_object; // Variable to hold the mapping object
    uint16 clear_val = 0x0000; // Value to clear the mapping

    for(int i = 1; i <= ec_slavecount; i++) { // Loop through each slave
        // 1. First, disable PDO
        retval += ec_SDOwrite(i, 0x1600, 0x00, FALSE, sizeof(zero_map), &zero_map, EC_TIMEOUTSAFE);
        
        // 2. Configure new PDO mapping for CSV mode
        // Add Control Word
        map_object = 0x60400010;  // 0x6040:0 Control Word, 16 bits
        retval += ec_SDOwrite(i, 0x1600, 0x01, FALSE, sizeof(map_object), &map_object, EC_TIMEOUTSAFE);
        
        // Add Target Velocity
        map_object = 0x60FF0020;  // 0x60FF:0 Target Velocity, 32 bits
        retval += ec_SDOwrite(i, 0x1600, 0x02, FALSE, sizeof(map_object), &map_object, EC_TIMEOUTSAFE);
        
        // Add Modes of Operation
        map_object = 0x60600008;  // 0x6060:0 Modes of Operation, 8 bits
        retval += ec_SDOwrite(i, 0x1600, 0x03, FALSE, sizeof(map_object), &map_object, EC_TIMEOUTSAFE);
        
        // Add 8-bit padding
        map_object = 0x00000008;  // 8-bit padding
        retval += ec_SDOwrite(i, 0x1600, 0x04, FALSE, sizeof(map_object), &map_object, EC_TIMEOUTSAFE);
        
        // Set the number of mapped objects (4 objects, including padding)
        uint8 map_count = 4;
        retval += ec_SDOwrite(i, 0x1600, 0x00, FALSE, sizeof(map_count), &map_count, EC_TIMEOUTSAFE);
        
        // 4. Configure RXPDO allocation
        clear_val = 0x0000; // Clear the mapping
        retval += ec_SDOwrite(i, 0x1c12, 0x00, FALSE, sizeof(clear_val), &clear_val, EC_TIMEOUTSAFE);
        map_1c12 = 0x1600; // Set the mapping to the new PDO
        retval += ec_SDOwrite(i, 0x1c12, 0x01, FALSE, sizeof(map_1c12), &map_1c12, EC_TIMEOUTSAFE);
        map_1c12 = 0x0001; // Set the mapping index
        retval += ec_SDOwrite(i, 0x1c12, 0x00, FALSE, sizeof(map_1c12), &map_1c12, EC_TIMEOUTSAFE);
    }

    printf("PDO mapping configuration result: %d\n", retval);
    if (retval < 0) {
        printf("PDO mapping failed\n");
        return -1;
    }

    printf("RXPOD Mapping set correctly.\n");
    printf("___________________________________________\n");

    //........................................................................................
    // Map TXPOD
    retval = 0;
    uint16 map_1c13;
    for(int i = 1; i <= ec_slavecount; i++) {
        // First, clear the TXPDO mapping
        clear_val = 0x0000;
        retval += ec_SDOwrite(i, 0x1A00, 0x00, FALSE, sizeof(clear_val), &clear_val, EC_TIMEOUTSAFE);

        // Configure TXPDO mapping entries
        // Status Word (0x6041:0, 16 bits)
        map_object = 0x60410010;
        retval += ec_SDOwrite(i, 0x1A00, 0x01, FALSE, sizeof(map_object), &map_object, EC_TIMEOUTSAFE);

        // Actual Position (0x6064:0, 32 bits)
        map_object = 0x60640020;
        retval += ec_SDOwrite(i, 0x1A00, 0x02, FALSE, sizeof(map_object), &map_object, EC_TIMEOUTSAFE);

        // Actual Velocity (0x606C:0, 32 bits)
        map_object = 0x606C0020;
        retval += ec_SDOwrite(i, 0x1A00, 0x03, FALSE, sizeof(map_object), &map_object, EC_TIMEOUTSAFE);

        // Actual Torque (0x6077:0, 16 bits)
        map_object = 0x60770010;
        retval += ec_SDOwrite(i, 0x1A00, 0x04, FALSE, sizeof(map_object), &map_object, EC_TIMEOUTSAFE);

        // Set the number of mapped objects (4 objects)
        uint8 map_count = 4;
        retval += ec_SDOwrite(i, 0x1A00, 0x00, FALSE, sizeof(map_count), &map_count, EC_TIMEOUTSAFE);

        // Configure TXPDO assignment
        // First, clear the assignment
        clear_val = 0x0000;
        retval += ec_SDOwrite(i, 0x1C13, 0x00, FALSE, sizeof(clear_val), &clear_val, EC_TIMEOUTSAFE);

        // Assign TXPDO to 0x1A00
        map_1c13 = 0x1A00;
        retval += ec_SDOwrite(i, 0x1C13, 0x01, FALSE, sizeof(map_1c13), &map_1c13, EC_TIMEOUTSAFE);

        // Set the number of assigned PDOs (1 PDO)
        map_1c13 = 0x0001;
        retval += ec_SDOwrite(i, 0x1C13, 0x00, FALSE, sizeof(map_1c13), &map_1c13, EC_TIMEOUTSAFE);
    }

    printf("Slave %d TXPDO mapping configuration result: %d\n", SLAVE_ID, retval);

    if (retval < 0) {
        printf("TXPDO Mapping failed\n");
        printf("___________________________________________\n");
        return -1;
    }

    printf("TXPDO Mapping set successfully\n");
    printf("___________________________________________\n");

   //##################################################################################################

    //4.- Set ecx_context.manualstatechange = 1. Map PDOs for all slaves by calling ec_config_map().
   printf("__________STEP 4___________________\n");

   ecx_context.manualstatechange = 1; //Disable automatic state change
   osal_usleep(1e6); //Sleep for 1 second

    uint8 WA = 0; //Variable for write access
    uint8 my_RA = 0; //Variable for read access
    uint32 TIME_RA; //Variable for time read access

    // Print the information of the slaves found
    for (int i = 1; i <= ec_slavecount; i++) {
       // (void)ecx_FPWR(ecx_context.port, i, ECT_REG_DCSYNCACT, sizeof(WA), &WA, 5 * EC_TIMEOUTRET);
        printf("Name: %s\n", ec_slave[i].name); //Print the name of the slave
        printf("Slave %d: Type %d, Address 0x%02x, State Machine actual %d, required %d\n", 
               i, ec_slave[i].eep_id, ec_slave[i].configadr, ec_slave[i].state, EC_STATE_INIT);
        printf("___________________________________________\n");
        ecx_dcsync0(&ecx_context, i, TRUE, 1000000, 0);  //Synchronize the distributed clock for the slave
    }

    // Map the configured PDOs to the IOmap
    ec_config_map(&IOmap);

    printf("__________STEP 5___________________\n");

    // Ensure all slaves are in PRE-OP state
    for(int i = 1; i <= ec_slavecount; i++) {
        if(ec_slave[i].state != EC_STATE_PRE_OP) { // Check if the slave is not in PRE-OP state
            printf("Slave %d not in PRE-OP state. Current state: %d\n", i, ec_slave[i].state);
            return -1; // Return error if any slave is not in PRE-OP state
        }
    }

    // Configure Distributed Clock (DC)
    ec_configdc(); // Set up the distributed clock for synchronization

    // Request to switch to SAFE-OP state
    ec_slave[0].state = EC_STATE_SAFE_OP; // Set the first slave to SAFE-OP state
    ec_writestate(0); // Write the state change to the slave

    // Wait for the state transition
    if (ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4) == EC_STATE_SAFE_OP) {
        printf("Successfully changed to SAFE_OP state\n"); // Confirm successful state change
    } else {
        printf("Failed to change to SAFE_OP state\n");
        return -1; // Return error if state change fails
    }

    // Calculate the expected Work Counter (WKC)
    expectedWKC = (ec_group[0].outputsWKC * 2) + ec_group[0].inputsWKC; // Calculate expected WKC based on outputs and inputs
    printf("Calculated workcounter %d\n", expectedWKC);

    // Read and display basic status information of the slaves
    ec_readstate(); // Read the state of all slaves
    for(int i = 1; i <= ec_slavecount; i++) {
        printf("Slave %d\n", i);
        printf("  State: %02x\n", ec_slave[i].state); // Print the state of the slave
        printf("  ALStatusCode: %04x\n", ec_slave[i].ALstatuscode); // Print the AL status code
        printf("  Delay: %d\n", ec_slave[i].pdelay); // Print the delay of the slave
        printf("  Has DC: %d\n", ec_slave[i].hasdc); // Check if the slave supports Distributed Clock
        printf("  DC Active: %d\n", ec_slave[i].DCactive); // Check if DC is active for the slave
        printf("  DC supported: %d\n", ec_slave[i].hasdc); // Print if DC is supported
    }

    // Read DC synchronization configuration using the correct parameters
    for(int i = 1; i <= ec_slavecount; i++) {
        uint16_t dcControl = 0; // Variable to hold DC control configuration
        int32_t cycleTime = 0; // Variable to hold cycle time
        int32_t shiftTime = 0; // Variable to hold shift time
        int size; // Variable to hold size for reading

        // Read DC synchronization configuration, adding the correct size parameter
        size = sizeof(dcControl);
        if (ec_SDOread(i, 0x1C32, 0x01, FALSE, &size, &dcControl, EC_TIMEOUTSAFE) > 0) {
            printf("Slave %d DC Configuration:\n", i);
            printf("  DC Control: 0x%04x\n", dcControl); // Print the DC control configuration
            
            size = sizeof(cycleTime);
            if (ec_SDOread(i, 0x1C32, 0x02, FALSE, &size, &cycleTime, EC_TIMEOUTSAFE) > 0) {
                printf("  Cycle Time: %d ns\n", cycleTime); // Print the cycle time
            }

        }
    }

    printf("__________STEP 6___________________\n");

    // Start the EtherCAT thread for real-time processing
    start_ecatthread_thread = TRUE; // Flag to indicate that the EtherCAT thread should start
    osal_thread_create_rt(&thread1, stack64k * 2, (void *)&ecatthread, (void *)&ctime_thread); // Create the real-time EtherCAT thread
    // set_thread_affinity(*thread1, 4); // Optional: Set CPU affinity for the thread
    osal_thread_create(&thread2, stack64k * 2, (void *)&ecatcheck, NULL); // Create the EtherCAT check thread
    // set_thread_affinity(*thread2, 5); // Optional: Set CPU affinity for the thread
    printf("___________________________________________\n");

    my_RA = 0; // Reset read access variable
    for (int cnt = 1; cnt <= ec_slavecount; cnt++) { // Loop through each slave
        ec_SDOread(cnt, 0x1c32, 0x01, FALSE, &rdl, &data_R, EC_TIMEOUTSAFE); // Read DC synchronization data
        printf("Slave %d DC synchronized 0x1c32: %d\n", cnt, data_R); // Print the synchronization data for each slave
    }

    // 8. Transition to OP state
    printf("__________STEP 8___________________\n");

    // Send process data to the slaves
    ec_send_processdata();
    wkc = ec_receive_processdata(EC_TIMEOUTRET); // Receive process data and store the Work Counter

    // Set the first slave to operational state
    ec_slave[0].state = EC_STATE_OPERATIONAL; // Change the state of the first slave to OP
    ec_writestate(0); // Write the state change to the slave

    // Wait for the state transition to complete
    if ((ec_statecheck(0, EC_STATE_OPERATIONAL, 5 * EC_TIMEOUTSTATE)) == EC_STATE_OPERATIONAL) {
        printf("State changed to EC_STATE_OPERATIONAL: %d\n", EC_STATE_OPERATIONAL); // Confirm successful state change
        printf("___________________________________________\n");
    } else {
        printf("State could not be changed to EC_STATE_OPERATIONAL\n"); // Error message if state change fails
        for (int cnt = 1; cnt <= ec_slavecount; cnt++) {
            printf("ALstatuscode: %d\n", ecx_context.slavelist[cnt].ALstatuscode); // Print AL status codes for each slave
        }
    }

    // Read and display the state of all slaves
    ec_readstate(); // Read the state of all slaves
    for (int i = 1; i <= ec_slavecount; i++) {
        printf("Slave %d: Type %d, Address 0x%02x, State Machine actual %d, required %d\n", 
               i, ec_slave[i].eep_id, ec_slave[i].configadr, ec_slave[i].state, EC_STATE_OPERATIONAL); // Print slave information
        printf("Name: %s\n", ec_slave[i].name); // Print the name of the slave
        printf("___________________________________________\n");
    }

    // 9. Configure servomotor and mode operation
    printf("__________STEP 9___________________\n");

    if (ec_slave[0].state == EC_STATE_OPERATIONAL) { // Check if the first slave is in OP state
        printf("######################################################################################\n");
        printf("################# All slaves entered into OP state! ##################################\n");
        
        // Initialize variables for PDO mapping
        uint16_t control_word = 0x0000;
        int32_t target_velocity = 0;
        uint32_t max_velocity = 50000;    // Maximum velocity limit
        int8_t op_mode = 9;            // CSV mode
        int8_t padding = 0;            // 8-bit padding

        // Impedance control parameters
        float M = 0.5;    // Virtual mass
        float B = 50.0;   // Damping coefficient
        float K = 100.0;  // Stiffness
        float dt = 0.001; // Control cycle time (1ms)
        
        // Variables for impedance control
        float current_velocity = 0.0;
        float desired_force = 0.0;  // Desired interaction force
        float force_error = 2.0;
        float velocity_cmd = 0.0;
        
        // 创建卡尔曼滤波器实例
        KalmanFilter velocity_filter(0.1, 1.0);  // Q=0.1, R=1.0

        for (int i = 1; i <= ec_slavecount; i++) {
            // Set operation mode to CSV
            ec_SDOwrite(i, 0x6060, 0x00, FALSE, sizeof(op_mode), &op_mode, EC_TIMEOUTSAFE);
            
            // Set maximum velocity
            ec_SDOwrite(i, 0x6080, 0x00, FALSE, sizeof(max_velocity), &max_velocity, EC_TIMEOUTSAFE);
            
            // Set initial control word
            ec_SDOwrite(i, 0x6040, 0x00, FALSE, sizeof(control_word), &control_word, EC_TIMEOUTSAFE);
        }

        // Record start time
        auto start = std::chrono::high_resolution_clock::now();
        int step = 0;

        // Main control loop
        for(i = 1; i <= 3 * 60 * 60 * 1000; i++) {
            ec_send_processdata();
            wkc = ec_receive_processdata(EC_TIMEOUTRET);

            if(wkc >= expectedWKC) {
                printf("Processdata cycle %4d, WKC %d\n", i, wkc);
                
                // Read TxPDO data and implement impedance control
                for(int slave = 1; slave <= ec_slavecount; slave++) {
                    uint8_t* input = ec_slave[slave].inputs;
                    
                    // Read status word (16 bits)
                    uint16_t statusword = *(uint16_t*)(input + 0);
                    
                    // Read actual position (32 bits)
                    int32_t actual_position = *(int32_t*)(input + 2);
                    
                    // Read actual velocity (32 bits)
                    int32_t actual_velocity = *(int32_t*)(input + 6);
                    
                    // Read actual torque (16 bits)
                    int16_t actual_torque = *(int16_t*)(input + 10);
                    
                    // Print status information
                    printf("Slave %d:\n", slave);
                    printf("  Status Word: 0x%04x\n", statusword);
                    printf("  Position: %d\n", actual_position);
                    printf("  Velocity: %d\n", actual_velocity);
                    printf("  Torque: %d\n", actual_torque);
                    
                    // Impedance control calculation
                    force_error = desired_force - actual_torque;  // Error between desired and actual force
                    
                    // Calculate velocity command based on impedance model
                    velocity_cmd = (force_error * dt*1000) / (M * dt + B)*1000;
                    
                    // 应用卡尔曼滤波
                    float filtered_velocity = velocity_filter.update(velocity_cmd);

                    // Limit velocity command
                    if(filtered_velocity > 10000) filtered_velocity = 10000;
                    if(filtered_velocity < -10000) filtered_velocity = -10000;
                    
                    target_velocity = (int32_t)filtered_velocity;
                    printf("  Filtered velocity: %f\n", filtered_velocity);
                }
                
                // Control logic based on the step counter
                if (step <= 200) {
                    // Initial state
                    control_word = 0x0080;
                    target_velocity = 0;
                    max_velocity = 500;
                    op_mode = 9;
                    padding = 0;
                } else if (step <= 300) {
                    // Shutdown command
                    control_word = 0x0006;
                } else if (step <= 400) {
                    // Switch On command
                    control_word = 0x0007;
                } else if (step <= 500) {
                    // Enable Operation command
                    control_word = 0x000F;
                } else {
                    // Normal operation
                    control_word = 0x000F;
            
                    max_velocity = 500;
                    op_mode = 9;
                    padding = 0;
                }

                // Send output data to each slave
                for (int slave = 1; slave <= ec_slavecount; slave++) {
                    uint8_t *output = ec_slave[slave].outputs;
                    
                    // Copy data with correct sizes and order matching PDO mapping
                    memcpy(output, &control_word, sizeof(uint16_t));      // 16-bit Control Word
                    memcpy(output + 2, &target_velocity, sizeof(int32_t));    // 32-bit Target Velocity
                    memcpy(output + 6, &op_mode, sizeof(int8_t));          // 8-bit Mode of Operation
                    memcpy(output + 7, &padding, sizeof(int8_t));          // 8-bit padding
                }
            }

            if(step < 900) {
                step += 1;
            }

            osal_usleep(1000); // 1ms cycle time
        }

        osal_usleep(1e6);

        ec_close();

         printf("\nRequest init state for all slaves\n");
         ec_slave[0].state = EC_STATE_INIT;
         /* request INIT state for all slaves */
         ec_writestate(0);

        printf("EtherCAT master closed.\n");
    } else {
        printf("Not all slaves reached operational state.\n");

        ec_readstate();
        for (int i = 1; i <= ec_slavecount; i++) {
            if (ec_slave[i].state != EC_STATE_OPERATIONAL) {
                printf("Slave %d State=0x%2.2x StatusCode=0x%4.4x : %s\n", i, ec_slave[i].state, ec_slave[i].ALstatuscode, ec_ALstatuscode2string(ec_slave[i].ALstatuscode));
                printf("Requesting INIT state for slave %d\n", i);
                ec_slave[i].state = EC_STATE_INIT;
                printf("___________________________________________\n");
            }
        }
    }

    return 0;
}

/* 
 * PI calculation to synchronize Linux time with the Distributed Clock (DC) time.
 * This function calculates the offset time needed to align the Linux time with the DC time.
 */
void ec_sync(int64 reftime, int64 cycletime, int64 *offsettime) {
    static int64 integral = 0; // Integral term for PI controller
    int64 delta; // Variable to hold the difference between reference time and cycle time
    delta = (reftime) % cycletime; // Calculate the delta time
    if (delta > (cycletime / 2)) {
        delta = delta - cycletime; // Adjust delta if it's greater than half the cycle time
    }
    if (delta > 0) {
        integral++; // Increment integral if delta is positive
    }
    if (delta < 0) {
        integral--; // Decrement integral if delta is negative
    }
    *offsettime = -(delta / 100) - (integral / 20); // Calculate the offset time
    gl_delta = delta; // Update global delta variable
}

/* 
 * Add nanoseconds to a timespec structure.
 * This function updates the timespec structure by adding a specified amount of time.
 */
void add_timespec(struct timespec *ts, int64 addtime) {
    int64 sec, nsec; // Variables to hold seconds and nanoseconds

    nsec = addtime % NSEC_PER_SEC; // Calculate nanoseconds to add
    sec = (addtime - nsec) / NSEC_PER_SEC; // Calculate seconds to add
    ts->tv_sec += sec; // Update seconds in timespec
    ts->tv_nsec += nsec; // Update nanoseconds in timespec
    if (ts->tv_nsec >= NSEC_PER_SEC) { // If nanoseconds exceed 1 second
        nsec = ts->tv_nsec % NSEC_PER_SEC; // Adjust nanoseconds
        ts->tv_sec += (ts->tv_nsec - nsec) / NSEC_PER_SEC; // Increment seconds
        ts->tv_nsec = nsec; // Set adjusted nanoseconds
    }
}

/* 
 * EtherCAT check thread function
 * This function monitors the state of the EtherCAT slaves and attempts to recover 
 * any slaves that are not in the operational state.
 */
OSAL_THREAD_FUNC ecatcheck(void *ptr) {
    int slave; // Variable to hold the current slave index
    (void)ptr; // Not used

    while (1) { // Infinite loop for monitoring
        if (inOP && ((wkc < expectedWKC) || ec_group[currentgroup].docheckstate)) {
            if (needlf) {
                needlf = FALSE; // Reset line feed flag
                printf("\n"); // Print a new line
            }
            ec_group[currentgroup].docheckstate = FALSE; // Reset check state
            ec_readstate(); // Read the state of all slaves
            for (slave = 1; slave <= ec_slavecount; slave++) { // Loop through each slave
                if ((ec_slave[slave].group == currentgroup) && (ec_slave[slave].state != EC_STATE_OPERATIONAL)) {
                    ec_group[currentgroup].docheckstate = TRUE; // Set check state if slave is not operational
                    if (ec_slave[slave].state == (EC_STATE_SAFE_OP + EC_STATE_ERROR)) {
                        printf("ERROR: Slave %d is in SAFE_OP + ERROR, attempting ack.\n", slave);
                        ec_slave[slave].state = (EC_STATE_SAFE_OP + EC_STATE_ACK); // Acknowledge error state
                        ec_writestate(slave); // Write the state change to the slave
                    } else if (ec_slave[slave].state == EC_STATE_SAFE_OP) {
                        printf("WARNING: Slave %d is in SAFE_OP, changing to OPERATIONAL.\n", slave);
                        ec_slave[slave].state = EC_STATE_OPERATIONAL; // Change state to operational
                        ec_writestate(slave); // Write the state change to the slave
                    } else if (ec_slave[slave].state > EC_STATE_NONE) {
                        if (ec_reconfig_slave(slave, EC_TIMEOUTMON)) { // Reconfigure the slave if needed
                            ec_slave[slave].islost = FALSE; // Mark slave as found
                            printf("MESSAGE: Slave %d reconfigured\n", slave);
                        }
                    } else if (!ec_slave[slave].islost) {
                        ec_statecheck(slave, EC_STATE_OPERATIONAL,  EC_TIMEOUTRET); // Check the state of the slave
                        if (ec_slave[slave].state == EC_STATE_NONE) {
                            ec_slave[slave].islost = TRUE; // Mark slave as lost
                            printf("ERROR: Slave %d lost\n", slave);
                        }
                    }
                }
                if (ec_slave[slave].islost) { // If the slave is marked as lost
                    if (ec_slave[slave].state == EC_STATE_NONE) {
                        if (ec_recover_slave(slave, EC_TIMEOUTMON)) { // Attempt to recover the lost slave
                            ec_slave[slave].islost = FALSE; // Mark slave as found
                            printf("MESSAGE: Slave %d recovered\n", slave);
                        }
                    } else {
                        ec_slave[slave].islost = FALSE; // Mark slave as found
                        printf("MESSAGE: Slave %d found\n", slave);
                    }
                }
            }
            if (!ec_group[currentgroup].docheckstate) {
                printf("OK: All slaves resumed OPERATIONAL.\n"); // Confirm all slaves are operational
            }
        }
    }
}

/* 
 * RT EtherCAT thread function
 * This function handles the real-time processing of EtherCAT data. 
 * It sends and receives process data in a loop, synchronizing with the 
 * distributed clock if available, and ensuring timely execution based on 
 * the specified cycle time.
 */
OSAL_THREAD_FUNC_RT ecatthread(void *ptr) {
    struct timespec ts, tleft; // Variables for time management
    int ht; // Variable for high-resolution time
    int64 cycletime; // Variable to hold the cycle time
    struct timeval tp; // Variable for time value

    // Get the current time in monotonic clock
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ht = (ts.tv_nsec / 1000000) + 1; /* Round to nearest ms */
    ts.tv_nsec = ht * 1000000; // Set nanoseconds to the rounded value
    if (ts.tv_nsec >= NSEC_PER_SEC) { // If nanoseconds exceed 1 second
        ts.tv_sec++; // Increment seconds
        ts.tv_nsec -= NSEC_PER_SEC; // Adjust nanoseconds
    }
    cycletime = *(int *)ptr * 1000; /* Convert cycle time from ms to ns */

    toff = 0; // Initialize time offset
    dorun = 0; // Initialize run flag
    ec_send_processdata(); // Send initial process data

    while (1) { // Infinite loop for real-time processing
        dorun++; // Increment run counter
        /* Calculate next cycle start */
        add_timespec(&ts, cycletime + toff); // Add cycle time to the current time
        /* Wait for the cycle start */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, &tleft); // Sleep until the next cycle

        if (start_ecatthread_thread) { // Check if the EtherCAT thread should run
            wkc = ec_receive_processdata(EC_TIMEOUTRET); // Receive process data and store the Work Counter

            if (ec_slave[0].hasdc) { // If the first slave supports Distributed Clock
                /* Calculate toff to synchronize Linux time with DC time */
                ec_sync(ec_DCtime, cycletime, &toff); // Synchronize time
            }

            ec_send_processdata(); // Send process data to the slaves
        }
    }
}

int correct_count = 0;
int incorrect_count = 0;
int test_count_sum = 100;
int test_count = 0;
float correct_rate = 0;

int main(int argc, char **argv) {
    needlf = FALSE;
    inOP = FALSE;
    start_ecatthread_thread = FALSE;
    dorun = 0;
    ctime_thread = 1000; // 1ms cycle time

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset); // Clear the CPU set
    CPU_SET(3, &cpuset); // Set CPU 0 (change this to a valid core number)

    // Set the CPU affinity for the current process
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == -1) {
        perror("sched_setaffinity");
        return EXIT_FAILURE;
    }

    // Your program logic here
    printf("Running on CPU core 0\n");

    erob_test();


    printf("End program\n");

    return EXIT_SUCCESS;
}