#include "FOC_Flash.h"
#include <string.h>


static FLASH_EraseInitTypeDef EraseInitStruct;
#define DATA_FLASH_PAGE 60
#define STORAGE_FLASH_BASE (0x08000000 + FLASH_PAGE_SIZE * DATA_FLASH_PAGE)
FLASH_StatusTypeDef FOC_FLASH_WriteData(FLASH_DataTypeDef *pdata){

    if(sizeof(FLASH_DataTypeDef) > FLASH_PAGE_SIZE){
        return FLASH_ERROR;
    }

    FLASH_DataTypeDef current_data;
    FOC_FLASH_ReadData(&current_data);
    if(memcmp(&current_data, pdata, sizeof(FLASH_DataTypeDef)) == 0){ // if data is the same, no need to write
        return FLASH_OK;
    }

    HAL_FLASH_Unlock();

    EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
    EraseInitStruct.Page = DATA_FLASH_PAGE;
    EraseInitStruct.NbPages = 1;

    uint32_t PAGEError = 0;
    if (HAL_FLASHEx_Erase(&EraseInitStruct, &PAGEError) != HAL_OK){
        return FLASH_ERROR;
    }

    

    for(uint32_t i = 0; i < sizeof(FLASH_DataTypeDef); i += sizeof(uint64_t)){
        uint64_t data_to_write = 0;
        memcpy(&data_to_write, ((uint8_t*)pdata) + i, sizeof(uint64_t));

        if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, STORAGE_FLASH_BASE + i, data_to_write) != HAL_OK){
            return FLASH_ERROR;
        } 
    }
    HAL_FLASH_Lock();
    return FLASH_OK;
}


FLASH_StatusTypeDef FOC_FLASH_ReadData(FLASH_DataTypeDef *pdata){
    for(uint32_t i = 0; i < sizeof(FLASH_DataTypeDef); i += sizeof(uint64_t)){
        uint64_t data64 = *(uint64_t*)(STORAGE_FLASH_BASE + i);

        memcpy((uint8_t*)pdata + i, &data64, sizeof(uint64_t));
    }
    
    return FLASH_OK;
}