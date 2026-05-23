#include "spi.h"


void spi2_init(void)
{
    esp_err_t ret = 0;
    spi_bus_config_t spi_bus_conf = {0};

    /* 配置SPI总线 */
    spi_bus_conf.mosi_io_num = SPI2_MOSI_PIN;// SPI总线的MOSI引脚
    spi_bus_conf.miso_io_num = SPI2_MISO_PIN;// SPI总线的MISO引脚
    spi_bus_conf.sclk_io_num = SPI2_CLK_PIN;// SPI总线的SCLK引脚
    spi_bus_conf.quadwp_io_num = -1;// SPI总线的WP引脚，设置为-1表示不使用
    spi_bus_conf.quadhd_io_num = -1;// SPI总线的HD引脚，设置为-1表示不使用
    spi_bus_conf.max_transfer_sz = 240 * 284 * 2;// SPI总线的最大传输大小，单位为字节，这里设置为240*284*2

    /* 初始化SPI总线 */
    ret = spi_bus_initialize(SPI2_HOST, &spi_bus_conf, SPI_DMA_CH_AUTO);// 初始化SPI总线，使用HSPI_HOST，传入配置结构体和自动选择DMA通道
    ESP_ERROR_CHECK(ret);// 检验参数值
}

// 发送命令
void spi2_write_cmd(spi_device_handle_t handle, uint8_t cmd)
{
    esp_err_t ret = 0;
    spi_transaction_t t = {0};// 定义一个SPI事务结构体，并初始化为0
    t.length = 8;// 设置事务的长度为8位，即1字节
    t.tx_buffer = &cmd;// 设置事务的发送缓冲区为cmd变量的地址
    ret = spi_device_polling_transmit(handle, &t);// 通过轮询方式发送SPI事务，传入设备句柄和事务结构体的地址
    ESP_ERROR_CHECK(ret);// 检验参数值

}


// 发送数据
void spi2_write_mydata(spi_device_handle_t handle, const uint8_t *data, int len)
{
    esp_err_t ret = 0;
    spi_transaction_t t = {0};// 定义一个SPI事务结构体，并初始化为0

    if(len == 0 || data == NULL) // 如果数据长度为0或者数据指针为NULL，直接返回，不进行传输
    {
        return;
    }

    t.length = len * 8;// 设置事务的长度为数据长度的8倍，即字节数转换为位数
    t.tx_buffer = data;// 设置事务的发送缓冲区为data指针
    t.rx_buffer = NULL;// 设置事务的接收缓冲区为NULL，表示不接收数据
    ret = spi_device_polling_transmit(handle, &t);// 通过轮询方式发送SPI事务，传入设备句柄和事务结构体的地址
    ESP_ERROR_CHECK(ret);// 检验参数值
}


// 发送一个字节并接收一个字节
uint8_t spi2_transfer_byte(spi_device_handle_t handle, uint8_t data)
{
    spi_transaction_t t = {0};// 定义一个SPI事务结构体，并初始化为0

    memset(&t, 0, sizeof(t));// 将事务结构体的内存清零

    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;// 设置事务的标志，表示使用tx_data和rx_data字段进行发送和接收
    t.tx_data[0] = data;// 将要发送的数据存储在tx_data数组的第一个元素中
    spi_device_transmit(handle, &t);// 发送SPI事务，传入设备句柄和事务结构体的地址

    return t.rx_data[0];// 返回接收到的数据，即rx_data数组的第一个元素
}

