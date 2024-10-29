#include "xaxidma.h" 
#include "xparameters.h"
#include "xil_exception.h"
#include "xscugic.h"
#include <stdio.h>
#include <xaxidma_hw.h> 
#include <xil_cache.h>
#include <xil_types.h>
#include <xstatus.h>



#define DMA_DEV_BASEADDR XPAR_AXI_DMA_0_BASEADDR
#define INTC_BASEADDR XPAR_XSCUGIC_0_BASEADDR

#define TX_INT_ID XPAR_FABRIC_AXI_DMA_0_INTR
#define RX_INT_ID XPAR_FABRIC_AXI_DMA_0_INTR_1

#define RESET_TIMEOUT_COUNTER 10000
#define TEST_START_VALUE 0x0
#define MAX_PKT_LEN 0x100
#define INT_OFFSET 32


#define DDR_BASE_ADDR 0x00000000
#define MEM_BASE_ADDR  (DDR_BASE_ADDR + 0x04400000)
#define TX_BUFFER_ADDR (MEM_BASE_ADDR + 0x00100000)
#define RX_BUFFER_ADDR (MEM_BASE_ADDR + 0x00300000)


static XAxiDma axidma_instance;
static XScuGic intrc_instance;

volatile int tx_done;
volatile int rx_done;
volatile int error;


static int setup_intr_system(XScuGic* intrc_ptr, XAxiDma* axidma_ptr);
static void disable_intr_system();
static void tx_intr_handler(void* callback);
static void rx_intr_handler(void* callback);

static int check_data(int length, u8 start_value);


int main(){
    int i;
    int status;

    u8 value;
    u8 *tx_buffer_ptr;
    u8 *rx_buffer_ptr;

    tx_buffer_ptr = (u8*) TX_BUFFER_ADDR;
    rx_buffer_ptr = (u8*) RX_BUFFER_ADDR;

    xil_printf("\r\n--- Entering main() --- \r\n");

    XAxiDma_Config* config = XAxiDma_LookupConfig(DMA_DEV_BASEADDR);

    if (!config) {
        xil_printf("No config found for %d\r\n", DMA_DEV_BASEADDR);
        return XST_FAILURE;
    }

    //初始化 DMA 引擎
    status = XAxiDma_CfgInitialize(&axidma_instance, config);
    if (status != XST_SUCCESS) {
        xil_printf("Initialization failed %d\r\n", status);
        return XST_FAILURE;
    }

    if (XAxiDma_HasSg(&axidma_instance)) {
        xil_printf("Device configured as SG mode \r\n");
        return XST_FAILURE;
    }

    xil_printf("raw tx_int_id:%x\r\n", config->IntrId[0]);
    xil_printf("raw rx_int_id:%x\r\n", config->IntrId[1]);
    xil_printf("parint: %x\r\n", (int)config->IntrParent);
    // xil_printf("is SGI PPI SPI:%x\r\n", XGet_IntrOffset(config->IntrId[0]));

    status = setup_intr_system(&intrc_instance, &axidma_instance);
    if (status != XST_SUCCESS){
        xil_printf("Failed intr setup\r\n");
        return XST_FAILURE;
    }

    tx_done = 0;
    rx_done = 0;
    error = 0;

    value = TEST_START_VALUE;
    for (i = 0; i < MAX_PKT_LEN; i++){
        tx_buffer_ptr[i] = value;
        value = (value + 1) & 0xFF;
    }

    xil_printf("waiting tx and rx....\r\n");


    Xil_DCacheFlushRange((UINTPTR)tx_buffer_ptr, MAX_PKT_LEN);
    xil_printf("1\r\n");

    status = XAxiDma_SimpleTransfer(&axidma_instance, \
    (UINTPTR)tx_buffer_ptr, MAX_PKT_LEN, XAXIDMA_DMA_TO_DEVICE);
    if (status != XST_SUCCESS) {
        return XST_FAILURE;
    }
    xil_printf("2\r\n");
    while (!tx_done && !error)
        ;
    xil_printf("3\r\n");
    if (error) {
        xil_printf("Failed test transmit%s done, "
        "receive%s done\r\n", tx_done ? "" : " not",
        rx_done ? "" : "not");
        goto Done;
    }

    status = XAxiDma_SimpleTransfer(&axidma_instance, \
    (UINTPTR)rx_buffer_ptr, MAX_PKT_LEN, XAXIDMA_DEVICE_TO_DMA);
    if (status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    if (error) {
        xil_printf("Failed test transmit%s done, "
        "receive%s done\r\n", tx_done ? "" : " not",
        rx_done ? "" : "not");
        goto Done;
    }

    while (!rx_done && !error)
        ;

    Xil_DCacheFlushRange((UINTPTR)rx_buffer_ptr, MAX_PKT_LEN);

    status = check_data(MAX_PKT_LEN, TEST_START_VALUE);
    if (status != XST_SUCCESS)
    {
        xil_printf("Data check Failed\r\n");
        goto Done;
    }

    xil_printf("Successfully ran AXI DMA Loop\r\n");
    disable_intr_system(config);


    Done: xil_printf("--- Exiting main() --- \r\n");

    return XST_SUCCESS;
}


static int setup_intr_system(XScuGic* intrc_ptr, XAxiDma* axidma_ptr){
    int status;
    XScuGic_Config* intrc_conf;
    xil_printf("init intr controller...\r\n");
    intrc_conf = XScuGic_LookupConfig(INTC_BASEADDR);
    if (NULL == intrc_conf){
        xil_printf("intrc conf failed\r\n");
        return XST_FAILURE;
    }
    xil_printf("get conf: %x\r\n", intrc_conf->DistBaseAddress);

    status = XScuGic_CfgInitialize(intrc_ptr, intrc_conf, intrc_conf->CpuBaseAddress);
    if (status != XST_SUCCESS) {
        xil_printf("intrc init failed %d\r\n", status);
        return XST_FAILURE;
    }
    xil_printf("setting intr type...\r\n");
    XScuGic_SetPriorityTriggerType(intrc_ptr, TX_INT_ID + INT_OFFSET, 0xA0, 0x3);
    XScuGic_SetPriorityTriggerType(intrc_ptr, RX_INT_ID + INT_OFFSET, 0xA0, 0x3);
    xil_printf("connect intr handler...\r\n");
    status = XScuGic_Connect(intrc_ptr, TX_INT_ID + INT_OFFSET, (Xil_ExceptionHandler)tx_intr_handler, axidma_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("TX INTR failed %d\r\n", status);
        return XST_FAILURE;
    }

    status = XScuGic_Connect(intrc_ptr, RX_INT_ID + INT_OFFSET, (Xil_ExceptionHandler)rx_intr_handler, axidma_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("RX INTR failed %d\r\n", status);
        return XST_FAILURE;
    }

    XScuGic_Enable(intrc_ptr, TX_INT_ID + INT_OFFSET);
    XScuGic_Enable(intrc_ptr, RX_INT_ID + INT_OFFSET);
    xil_printf("init hw exception...\r\n");
    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT, (Xil_ExceptionHandler)XScuGic_InterruptHandler, (void *)intrc_ptr);
    Xil_ExceptionEnable();



    XAxiDma_IntrEnable(axidma_ptr, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DMA_TO_DEVICE);
    XAxiDma_IntrEnable(axidma_ptr, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);


    return XST_SUCCESS;
}

static void disable_intr_system()
{
    // XDisconnectInterruptCntrl(rx_intr_id + INT_OFFSET, axi_conf_ptr->IntrParent);
    // XDisconnectInterruptCntrl(tx_intr_id + INT_OFFSET, axi_conf_ptr->IntrParent);

    XAxiDma_IntrDisable(&axidma_instance, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DMA_TO_DEVICE);
    XAxiDma_IntrDisable(&axidma_instance, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
}


static void tx_intr_handler(void* callback){

    printf("tx fired!\r\n");

    int timeout;
    u32 irq_status;
    XAxiDma* axidma_ptr = (XAxiDma*)callback;

    irq_status = XAxiDma_IntrGetIrq(axidma_ptr, XAXIDMA_DMA_TO_DEVICE);
    xil_printf("tx irq_status:%x\r\n", irq_status);
    XAxiDma_IntrAckIrq(axidma_ptr, 0x1000, XAXIDMA_DMA_TO_DEVICE);

    if ((irq_status & XAXIDMA_IRQ_ERROR_MASK)){ 
        error = 1;
        XAxiDma_Reset(axidma_ptr);
        timeout = RESET_TIMEOUT_COUNTER;
        while(timeout){
            if (XAxiDma_ResetIsDone(axidma_ptr))
                break;
            timeout -= 1;
        }
        return;
    }


    if ((irq_status & XAXIDMA_IRQ_IOC_MASK))
    {
        xil_printf("t done\r\n");
        tx_done = 1;  
    }
  

}
static void rx_intr_handler(void* callback){

    xil_printf("rx_intr_fired!\n\r");

    u32 irq_status;
    int timeout;
    XAxiDma *axidma_ptr = (XAxiDma*) callback;

    irq_status = XAxiDma_IntrGetIrq(axidma_ptr, XAXIDMA_DEVICE_TO_DMA);
    xil_printf("rx irq_status:%x\r\n", irq_status);
    XAxiDma_IntrAckIrq(axidma_ptr, 0x1000, XAXIDMA_DEVICE_TO_DMA);
    
    if ((irq_status & XAXIDMA_IRQ_ERROR_MASK)){
        
        error = 1;
        XAxiDma_Reset(axidma_ptr);
        timeout = RESET_TIMEOUT_COUNTER;
        while (timeout) {
            if (XAxiDma_ResetIsDone(axidma_ptr))
                break;
            timeout -= 1;
        }
        return;
    }

    if ((irq_status & XAXIDMA_IRQ_IOC_MASK)){
        xil_printf("rx_done\r\n");
        rx_done = 1;
    }

}


static int check_data(int length, u8 start_value){
    u8 value;
    u8 *rx_packet;
    int i = 0;

    value = start_value;
    rx_packet = (u8*) RX_BUFFER_ADDR;
    for (i = 0; i < length; i++){
        if (rx_packet[i] != value){
            xil_printf("Data error %d: %x/%x\r\n", i, rx_packet[i], value);
            return XST_FAILURE;
        }
        xil_printf("ttx:%x / rx:%x\r\n", value, rx_packet[i]);
        value = (value + 1) & 0xFF;
    }

    return XST_SUCCESS;
}