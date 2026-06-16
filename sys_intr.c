#include "sys_intr.h"   // 包含中断系统相关的头文件（含 XScuGic 等定义）

/**
 * @brief 安装 ARM 异常向量并全局使能中断
 * @param IntcInstancePtr 指向已配置的中断控制器实例（XScuGic）的指针
 * @note  该函数负责将 XScuGic 的中断处理函数挂接到 ARM 的 IRQ 异常向量上，
 *        完成这一步后，CPU 才能响应外设中断。
 */
void Setup_Intr_Exception(XScuGic * IntcInstancePtr)
{
	/* 初始化 ARM 异常处理系统（清空向量表等） */
	Xil_ExceptionInit();

	/* 注册中断异常处理函数：
	 * 当 ARM 发生 IRQ 异常时，跳转到 XScuGic_InterruptHandler 并传入
	 * IntcInstancePtr 作为参数，该函数会读取中断控制器状态并分发给具体外设。
	 */
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
			(Xil_ExceptionHandler)XScuGic_InterruptHandler,
			(void *)IntcInstancePtr);

	/* 使能 ARM 内核的 IRQ 中断（全局中断开关） */
	Xil_ExceptionEnable();
}

/**
 * @brief 初始化通用中断控制器（GIC）驱动实例
 * @param IntcInstancePtr 指向一个 XScuGic 实例的指针，函数将对此实例进行初始化
 * @return XST_SUCCESS 表示成功，XST_FAILURE 表示失败
 * @note  该函数根据设备 ID 查找硬件配置，并完成中断控制器的基础初始化。
 */
int Init_Intr_System(XScuGic * IntcInstancePtr)
{
	int Status;
	XScuGic_Config *IntcConfig;

	/*
	 * 根据设备 ID（INTC_DEVICE_ID）查找中断控制器的配置信息，
	 * 该 ID 通常定义在 xparameters.h 中。
	 */
	IntcConfig = XScuGic_LookupConfig(INTC_DEVICE_ID);
	if (NULL == IntcConfig) {
		// 未找到对应配置，返回失败
		return XST_FAILURE;
	}

	/*
	 * 使用查找到的配置信息初始化中断控制器实例，
	 * 包括设置寄存器基地址等。
	 */
	Status = XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig,
					IntcConfig->CpuBaseAddress);
	if (Status != XST_SUCCESS) {
		// 初始化失败
		return XST_FAILURE;
	}

	// 中断控制器驱动初始化成功
	return XST_SUCCESS;
}
