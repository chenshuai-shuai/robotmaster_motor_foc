#include "CRSF.h"
#include "crc8.h"


CRSF_CH_Struct CRSF_CH;

/**
 * @brief 用于接收处理小白控数据帧的回调
 * 
 * @param data 
 * @param len 
 */
void Crsf_Data_Read(uint8_t *data, uint8_t len)
{
    uint8_t inCrc = data[2 + len - 1];
    // CRC计算
    uint8_t crc = Crc8_calc(&data[2], len - 1);


    if( (data[2] == CRSF_FRAMETYPE_RC_CHANNELS_PACKED)&&(inCrc == crc) )
    {
        CRSF_CH.CH1  = ((int16_t)data[ 3] >> 0 | ((int16_t)data[ 4] << 8 )) & 0x07FF;
        CRSF_CH.CH2  = ((int16_t)data[ 4] >> 3 | ((int16_t)data[ 5] << 5 )) & 0x07FF;
        CRSF_CH.CH3  = ((int16_t)data[ 5] >> 6 | ((int16_t)data[ 6] << 2 ) | (int16_t)data[ 7] << 10 ) & 0x07FF;
        CRSF_CH.CH4  = ((int16_t)data[ 7] >> 1 | ((int16_t)data[ 8] << 7 )) & 0x07FF;
        CRSF_CH.CH5  = ((int16_t)data[ 8] >> 4 | ((int16_t)data[ 9] << 4 )) & 0x07FF;
        CRSF_CH.CH6  = ((int16_t)data[ 9] >> 7 | ((int16_t)data[10] << 1 ) | (int16_t)data[11] << 9 ) & 0x07FF;
        CRSF_CH.CH7  = ((int16_t)data[11] >> 2 | ((int16_t)data[12] << 6 )) & 0x07FF;
        CRSF_CH.CH8  = ((int16_t)data[12] >> 5 | ((int16_t)data[13] << 3 )) & 0x07FF;
        CRSF_CH.CH9  = ((int16_t)data[14] << 0 | ((int16_t)data[15] << 8 )) & 0x07FF;
        CRSF_CH.CH10 = ((int16_t)data[15] >> 3 | ((int16_t)data[16] << 5 )) & 0x07FF;
        CRSF_CH.CH11 = ((int16_t)data[16] >> 6 | ((int16_t)data[17] << 2 ) | (int16_t)data[18] << 10 ) & 0x07FF;
        CRSF_CH.CH12 = ((int16_t)data[18] >> 1 | ((int16_t)data[19] << 7 )) & 0x07FF;
        CRSF_CH.CH13 = ((int16_t)data[19] >> 4 | ((int16_t)data[20] << 4 )) & 0x07FF;
        CRSF_CH.CH14 = ((int16_t)data[20] >> 7 | ((int16_t)data[21] << 1 ) | (int16_t)data[22] << 9 ) & 0x07FF;
        CRSF_CH.CH15 = ((int16_t)data[22] >> 2 | ((int16_t)data[23] << 6 )) & 0x07FF;
        CRSF_CH.CH16 = ((int16_t)data[23] >> 5 | ((int16_t)data[24] << 3 )) & 0x07FF;

        CRSF_CH.ConnectState = SBUS_SIGNAL_OK;
    }

}

