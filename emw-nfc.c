#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <nfc/nfc.h>
#include <nfc/nfc_worker.h>
#include <nfc/nfc_listener.h>
#include <nfc/nfc_poller.h>
#include <storage/storage.h>
#include <stream/file_stream.h>
#include <lib/toolbox/path.h>

#define TAG "EMV_Emulator"
#define MAX_CARDS 10
#define SAVE_PATH "/ext/apps_data/emv_cards.bin"

// EMV AID (Application Identifier) definitions
#define AID_VISA "A0000000031010"
#define AID_MASTERCARD "A0000000041010"
#define AID_AMERICAN_EXPRESS "A00000002501"
#define AID_MAESTRO "A0000000043060"

typedef enum {
    CardTypeVisa,
    CardTypeMastercard,
    CardTypeAmex,
    CardTypeMaestro
} CardType;

typedef struct {
    uint8_t pan[16];        // Primary Account Number
    uint8_t exp_month;
    uint8_t exp_year;
    uint8_t cvv[3];
    uint32_t card_number;
    CardType type;
    char holder_name[26];
    bool is_contactless;
    uint8_t atc;           // Application Transaction Counter
    uint8_t un[6];         // Unpredictable Number
} EMVCard;

typedef struct {
    Gui* gui;
    NotificationApp* notification;
    FuriMutex* mutex;
    
    Nfc* nfc;
    NfcWorker* worker;
    NfcListener* listener;
    
    EMVCard cards[MAX_CARDS];
    int total_cards;
    int current_card_index;
    
    bool emulating;
    bool field_detected;
    uint32_t transaction_count;
    uint32_t last_interaction;
    bool show_details;
} EMVEmulatorApp;

static const uint8_t visa_aid[] = {0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10};
static const uint8_t mc_aid[] = {0xA0, 0x00, 0x00, 0x00, 0x04, 0x10, 0x10};

static void generate_luhn(uint8_t* digits, int length) {
    int sum = 0;
    bool double_digit = false;
    
    for(int i = length - 2; i >= 0; i--) {
        int digit = digits[i];
        if(double_digit) {
            digit *= 2;
            if(digit > 9) digit -= 9;
        }
        sum += digit;
        double_digit = !double_digit;
    }
    
    int check_digit = (10 - (sum % 10)) % 10;
    digits[length - 1] = check_digit;
}

static void generate_card_number(EMVCard* card) {
    // Generate 16-digit card number based on card type
    uint8_t digits[16];
    
    switch(card->type) {
        case CardTypeVisa:
            digits[0] = 4; // Visa starts with 4
            break;
        case CardTypeMastercard:
            digits[0] = 5; // Mastercard starts with 5
            digits[1] = 1 + (furi_get_tick() % 5); // 51-55
            break;
        case CardTypeAmex:
            digits[0] = 3; // Amex starts with 3
            digits[1] = 4 + (furi_get_tick() % 2); // 34 or 37
            break;
        default:
            digits[0] = 6; // Maestro starts with 6
            break;
    }
    
    // Fill remaining digits
    for(int i = 2; i < 15; i++) {
        digits[i] = furi_get_tick() % 10;
    }
    
    generate_luhn(digits, 16);
    
    // Convert to BCD
    for(int i = 0; i < 8; i++) {
        card->pan[i] = (digits[i * 2] << 4) | digits[i * 2 + 1];
    }
}

static void init_emv_card(EMVCard* card, CardType type) {
    memset(card, 0, sizeof(EMVCard));
    card->type = type;
    card->is_contactless = true;
    card->atc = 0;
    
    generate_card_number(card);
    
    // Set expiry (2 years from now)
    uint32_t current_year = 2024 + (furi_get_tick() / (365 * 24 * 3600 * 1000)) % 10;
    card->exp_year = (current_year + 2) % 100;
    card->exp_month = 1 + (furi_get_tick() % 12);
    
    // Generate CVV
    for(int i = 0; i < 3; i++) {
        card->cvv[i] = 1 + (furi_get_tick() % 9);
    }
    
    // Generate cardholder name
    const char* first_names[] = {"JOHN", "MARY", "DAVID", "SARAH", "MICHAEL", "EMMA"};
    const char* last_names[] = {"SMITH", "JOHNSON", "WILLIAMS", "BROWN", "JONES", "GARCIA"};
    
    snprintf(card->holder_name, sizeof(card->holder_name), "%s/%s",
             first_names[furi_get_tick() % COUNT_OF(first_names)],
             last_names[furi_get_tick() % COUNT_OF(last_names)]);
}

static uint16_t calculate_crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    
    for(size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for(int j = 0; j < 8; j++) {
            if(crc & 1) {
                crc = (crc >> 1) ^ 0x8408;
            } else {
                crc >>= 1;
            }
        }
    }
    
    return crc;
}

static void process_apdu_command(EMVEmulatorApp* app, const uint8_t* apdu, size_t length, 
                                 uint8_t* response, size_t* response_length) {
    if(length < 4) return;
    
    uint8_t cla = apdu[0];
    uint8_t ins = apdu[1];
    uint8_t p1 = apdu[2];
    uint8_t p2 = apdu[3];
    
    EMVCard* card = &app->cards[app->current_card_index];
    
    switch(ins) {
        case 0xA4: // SELECT
            if(length >= 5 && apdu[4] == 0x07) {
                // AID selection
                if(memcmp(&apdu[5], visa_aid, 7) == 0 || 
                   memcmp(&apdu[5], mc_aid, 7) == 0) {
                    response[0] = 0x61; // SW1
                    response[1] = 0x1C; // SW2 (more data available)
                    *response_length = 2;
                } else {
                    response[0] = 0x6A; // SW1
                    response[1] = 0x82; // SW2 (not found)
                    *response_length = 2;
                }
            }
            break;
            
        case 0xB2: // READ RECORD
            response[0] = 0x6C; // SW1
            response[1] = 0x20; // SW2 (Le)
            *response_length = 2;
            break;
            
        case 0x80: // GENERATE AC
            // Increment ATC for each transaction
            card->atc++;
            app->transaction_count++;
            
            // Generate unpredictable number
            for(int i = 0; i < 6; i++) {
                card->un[i] = furi_get_tick() % 256;
            }
            
            // Return cryptogram
            response[0] = 0x77; // BER-TLV response template
            response[1] = 0x1A; // Length
            response[2] = 0x9F; // Cryptogram Information Data
            response[3] = 0x27;
            response[4] = 0x01;
            response[5] = 0x80; // ARQC
            response[6] = 0x9F; // ATC
            response[7] = 0x36;
            response[8] = 0x02;
            response[9] = 0x00;
            response[10] = card->atc;
            response[11] = 0x9F; // Unpredictable Number
            response[12] = 0x37;
            response[13] = 0x06;
            memcpy(&response[14], card->un,