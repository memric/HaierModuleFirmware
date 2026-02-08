#include "unity.h"
#include "haier.h"

const uint8_t state_request[] = {0xFF, 0xFF, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x4D, 0x01, 0x5A};

extern uint8_t Haier_ProtocolGetChecksum(const uint8_t *data, uint16_t len);
extern uint8_t Haier_PrepareRequest(uint8_t *data, const uint8_t cmd);

TEST_CASE("Haier CRC", "[passed]")
{
    const uint8_t calculated_crc = Haier_ProtocolGetChecksum(&state_request[2], sizeof(state_request) - 2 - 1);

    TEST_ASSERT_EQUAL(calculated_crc, state_request[sizeof(state_request) - 1]);
}

TEST_CASE("Haier Request", "[passed]")
{
    uint8_t test_data[128] = {0};

    const uint8_t len = Haier_PrepareRequest(test_data, 1U); // State request

    TEST_ASSERT_EQUAL(len, 10U);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(state_request, test_data, len);
}