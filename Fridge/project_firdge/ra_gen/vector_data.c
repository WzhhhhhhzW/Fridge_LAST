/* generated vector source file - do not edit */
#include "bsp_api.h"
/* Do not build these data structures if no interrupts are currently allocated because IAR will have build errors. */
#if VECTOR_DATA_IRQ_COUNT > 0
        BSP_DONT_REMOVE const fsp_vector_t g_vector_table[BSP_ICU_VECTOR_NUM_ENTRIES] BSP_PLACE_IN_SECTION(BSP_SECTION_APPLICATION_VECTORS) =
        {
                        [0] = sci_uart_rxi_isr, /* SCI7 RXI (Receive data full) */
            [1] = sci_uart_txi_isr, /* SCI7 TXI (Transmit data empty) */
            [2] = sci_uart_tei_isr, /* SCI7 TEI (Transmit end) */
            [3] = sci_uart_eri_isr, /* SCI7 ERI (Receive error) */
            [4] = sci_uart_rxi_isr, /* SCI2 RXI (Receive data full) */
            [5] = sci_uart_txi_isr, /* SCI2 TXI (Transmit data empty) */
            [6] = sci_uart_tei_isr, /* SCI2 TEI (Transmit end) */
            [7] = sci_uart_eri_isr, /* SCI2 ERI (Receive error) */
            [8] = sci_uart_rxi_isr, /* SCI4 RXI (Receive data full) */
            [9] = sci_uart_txi_isr, /* SCI4 TXI (Transmit data empty) */
            [10] = sci_uart_tei_isr, /* SCI4 TEI (Transmit end) */
            [11] = sci_uart_eri_isr, /* SCI4 ERI (Receive error) */
            [12] = sci_uart_rxi_isr, /* SCI5 RXI (Receive data full) */
            [13] = sci_uart_txi_isr, /* SCI5 TXI (Transmit data empty) */
            [14] = sci_uart_tei_isr, /* SCI5 TEI (Transmit end) */
            [15] = sci_uart_eri_isr, /* SCI5 ERI (Receive error) */
            [16] = gpt_counter_overflow_isr, /* GPT3 COUNTER OVERFLOW (Overflow) */
            [17] = sci_uart_rxi_isr, /* SCI6 RXI (Receive data full) */
            [18] = sci_uart_txi_isr, /* SCI6 TXI (Transmit data empty) */
            [19] = sci_uart_tei_isr, /* SCI6 TEI (Transmit end) */
            [20] = sci_uart_eri_isr, /* SCI6 ERI (Receive error) */
            [21] = iic_master_rxi_isr, /* IIC1 RXI (Receive data full) */
            [22] = iic_master_txi_isr, /* IIC1 TXI (Transmit data empty) */
            [23] = iic_master_tei_isr, /* IIC1 TEI (Transmit end) */
            [24] = iic_master_eri_isr, /* IIC1 ERI (Transfer error) */
            [25] = gpt_counter_overflow_isr, /* GPT5 COUNTER OVERFLOW (Overflow) */
            [26] = ssi_txi_isr, /* SSI0 TXI (Transmit data empty) */
            [27] = ssi_rxi_isr, /* SSI0 RXI (Receive data full) */
            [28] = ssi_int_isr, /* SSI0 INT (Error interrupt) */
            [29] = spi_rxi_isr, /* SPI0 RXI (Receive buffer full) */
            [30] = spi_txi_isr, /* SPI0 TXI (Transmit buffer empty) */
            [31] = spi_tei_isr, /* SPI0 TEI (Transmission complete event) */
            [32] = spi_eri_isr, /* SPI0 ERI (Error) */
        };
        #if BSP_FEATURE_ICU_HAS_IELSR
        const bsp_interrupt_event_t g_interrupt_event_link_select[BSP_ICU_VECTOR_NUM_ENTRIES] =
        {
            [0] = BSP_PRV_VECT_ENUM(EVENT_SCI7_RXI,GROUP0), /* SCI7 RXI (Receive data full) */
            [1] = BSP_PRV_VECT_ENUM(EVENT_SCI7_TXI,GROUP1), /* SCI7 TXI (Transmit data empty) */
            [2] = BSP_PRV_VECT_ENUM(EVENT_SCI7_TEI,GROUP2), /* SCI7 TEI (Transmit end) */
            [3] = BSP_PRV_VECT_ENUM(EVENT_SCI7_ERI,GROUP3), /* SCI7 ERI (Receive error) */
            [4] = BSP_PRV_VECT_ENUM(EVENT_SCI2_RXI,GROUP4), /* SCI2 RXI (Receive data full) */
            [5] = BSP_PRV_VECT_ENUM(EVENT_SCI2_TXI,GROUP5), /* SCI2 TXI (Transmit data empty) */
            [6] = BSP_PRV_VECT_ENUM(EVENT_SCI2_TEI,GROUP6), /* SCI2 TEI (Transmit end) */
            [7] = BSP_PRV_VECT_ENUM(EVENT_SCI2_ERI,GROUP7), /* SCI2 ERI (Receive error) */
            [8] = BSP_PRV_VECT_ENUM(EVENT_SCI4_RXI,GROUP0), /* SCI4 RXI (Receive data full) */
            [9] = BSP_PRV_VECT_ENUM(EVENT_SCI4_TXI,GROUP1), /* SCI4 TXI (Transmit data empty) */
            [10] = BSP_PRV_VECT_ENUM(EVENT_SCI4_TEI,GROUP2), /* SCI4 TEI (Transmit end) */
            [11] = BSP_PRV_VECT_ENUM(EVENT_SCI4_ERI,GROUP3), /* SCI4 ERI (Receive error) */
            [12] = BSP_PRV_VECT_ENUM(EVENT_SCI5_RXI,GROUP4), /* SCI5 RXI (Receive data full) */
            [13] = BSP_PRV_VECT_ENUM(EVENT_SCI5_TXI,GROUP5), /* SCI5 TXI (Transmit data empty) */
            [14] = BSP_PRV_VECT_ENUM(EVENT_SCI5_TEI,GROUP6), /* SCI5 TEI (Transmit end) */
            [15] = BSP_PRV_VECT_ENUM(EVENT_SCI5_ERI,GROUP7), /* SCI5 ERI (Receive error) */
            [16] = BSP_PRV_VECT_ENUM(EVENT_GPT3_COUNTER_OVERFLOW,GROUP0), /* GPT3 COUNTER OVERFLOW (Overflow) */
            [17] = BSP_PRV_VECT_ENUM(EVENT_SCI6_RXI,GROUP1), /* SCI6 RXI (Receive data full) */
            [18] = BSP_PRV_VECT_ENUM(EVENT_SCI6_TXI,GROUP2), /* SCI6 TXI (Transmit data empty) */
            [19] = BSP_PRV_VECT_ENUM(EVENT_SCI6_TEI,GROUP3), /* SCI6 TEI (Transmit end) */
            [20] = BSP_PRV_VECT_ENUM(EVENT_SCI6_ERI,GROUP4), /* SCI6 ERI (Receive error) */
            [21] = BSP_PRV_VECT_ENUM(EVENT_IIC1_RXI,GROUP5), /* IIC1 RXI (Receive data full) */
            [22] = BSP_PRV_VECT_ENUM(EVENT_IIC1_TXI,GROUP6), /* IIC1 TXI (Transmit data empty) */
            [23] = BSP_PRV_VECT_ENUM(EVENT_IIC1_TEI,GROUP7), /* IIC1 TEI (Transmit end) */
            [24] = BSP_PRV_VECT_ENUM(EVENT_IIC1_ERI,GROUP0), /* IIC1 ERI (Transfer error) */
            [25] = BSP_PRV_VECT_ENUM(EVENT_GPT5_COUNTER_OVERFLOW,GROUP1), /* GPT5 COUNTER OVERFLOW (Overflow) */
            [26] = BSP_PRV_VECT_ENUM(EVENT_SSI0_TXI,GROUP2), /* SSI0 TXI (Transmit data empty) */
            [27] = BSP_PRV_VECT_ENUM(EVENT_SSI0_RXI,GROUP3), /* SSI0 RXI (Receive data full) */
            [28] = BSP_PRV_VECT_ENUM(EVENT_SSI0_INT,GROUP4), /* SSI0 INT (Error interrupt) */
            [29] = BSP_PRV_VECT_ENUM(EVENT_SPI0_RXI,GROUP5), /* SPI0 RXI (Receive buffer full) */
            [30] = BSP_PRV_VECT_ENUM(EVENT_SPI0_TXI,GROUP6), /* SPI0 TXI (Transmit buffer empty) */
            [31] = BSP_PRV_VECT_ENUM(EVENT_SPI0_TEI,GROUP7), /* SPI0 TEI (Transmission complete event) */
            [32] = BSP_PRV_VECT_ENUM(EVENT_SPI0_ERI,FIXED), /* SPI0 ERI (Error) */
        };
        #endif
        #endif
