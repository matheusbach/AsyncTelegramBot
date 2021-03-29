#include "AsyncTelegram2.h"

#if DEBUG_ENABLE
#define debugJson(X, Y)  { log_debug(); Serial.println(); serializeJsonPretty(X, Y); Serial.println();}
#define errorJson(E)  { log_error(); Serial.println(); Serial.println(E);}
#else
#define debugJson(X, Y)
#define errorJson(E)
#endif

AsyncTelegram2::AsyncTelegram2(Client &client)
{
    m_botusername.reserve(32); // Telegram username is 5-32 chars lenght
    telegramClient = &client;
    m_minUpdateTime = MIN_UPDATE_TIME;
}

AsyncTelegram2::~AsyncTelegram2() {};


bool AsyncTelegram2::checkConnection()
{
    static uint32_t lastCTime = millis();
    // Start connection with Telegramn server (if necessary)
    if (!telegramClient->connected()) {
        if (!telegramClient->connect(TELEGRAM_HOST, TELEGRAM_PORT)) {
            Serial.printf("\n\nUnable to connect to Telegram server\n");
        }
        else {
            log_debug("Connected using Telegram hostname\n"
                      "Last connection was %d seconds ago\n",
                      (int)(millis() - lastCTime)/1000);
            lastCTime = millis();
        }
    }
    return telegramClient->connected();
}


bool AsyncTelegram2::begin()
{
    checkConnection();
    return getMe();
}


bool AsyncTelegram2::reset(void)
{
    log_debug("Restart Telegram connection\n");
    telegramClient->stop();
    m_lastmsg_timestamp = millis();
    m_waitingReply = false;
    return checkConnection();
}


// Blocking https POST to server (used with ESP8266)
bool AsyncTelegram2::sendCommand(const char* const &command, JsonDocument &doc, bool blocking )
{
    if(checkConnection()) {
        // JsonDocument doc is used as input for request preparation and then reused as output result
        String request((char *)0);
        request.reserve(BUFFER_BIG);
        request = "POST https://" TELEGRAM_HOST "/bot";
        request += m_token;
        request += "/";
        request += command;
        // Let's use 1.0 protocol in order to avoid chunked transfer encoding
        request += " HTTP/1.0" "\nHost: api.telegram.org" "\nConnection: keep-alive" "\nContent-Type: application/json";
        request += "\nContent-Length: ";
        request += measureJson(doc);
        request += "\n\n";
        request += doc.as<String>();

        telegramClient->print(request);
        // Serializing to stream don't work well as expected.
        // telegramClient->write((uint8_t*) doc.as<String>().c_str(), measureJson(doc));
        m_waitingReply = true;

        // Blocking mode
        if (blocking) {
            String payload((char *)0);
            payload.reserve(BUFFER_BIG);
            if (telegramClient->connected()) {
                yield();
                // skip headers
                telegramClient->find("\r\n\r\n");
            }
            // If there are incoming bytes available from the server, read them and print them:
            while (telegramClient->available()) {
                yield();
                payload  += (char) telegramClient->read();
            }
            m_waitingReply = false;
            doc.clear();
            deserializeJson(doc, payload);
            return true;
        }
    }
    return false;
}



bool AsyncTelegram2::getUpdates(JsonDocument &doc){
    // No response from Telegram server for a long time
    if(millis() - m_lastmsg_timestamp > 10*m_minUpdateTime) {
        reset();
    }

    // Send message to Telegram server only if enough time has passed since last
    if(millis() - m_lastUpdateTime > m_minUpdateTime){
        m_lastUpdateTime = millis();

        // If previuos reply from server was received (and parsed)
        if( m_waitingReply == false ) {
            StaticJsonDocument<BUFFER_SMALL> smallDoc;
            smallDoc["limit"] = 1;
            smallDoc["timeout"] = 0;    // polling timeout: add &timeout=<seconds. zero for short polling.
            smallDoc["allowed_updates"] = "message,callback_query,inline_query";
            if (m_lastUpdateId != 0) {
                smallDoc["offset"] = m_lastUpdateId;
            }
            sendCommand("getUpdates", smallDoc);
        }
    }

    String payload((char *)0);
    if(telegramClient->connected() && telegramClient->available()) {
        payload.reserve(BUFFER_BIG);
        // Skip headers
        telegramClient->find("\r\n\r\n");
        // If there are incoming bytes available from the server, read them and store:
        while (telegramClient->available() ){
            yield();
            payload += (char) telegramClient->read();
        }
    }
    m_lastmsg_timestamp = millis();
    m_waitingReply = false;
    // We have a message, parse data received
    DeserializationError err = deserializeJson(doc, payload);
    return (err == 0 && doc.containsKey("ok"));
}


// Parse message received from Telegram server
MessageType AsyncTelegram2::getNewMessage(TBMessage &message )
{
    message.messageType = MessageNoData;
    DynamicJsonDocument root(BUFFER_MEDIUM);
    // We have a message, parse data received
    if (getUpdates(root)) {
        root.shrinkToFit();
        if (!root.containsKey("ok")) {
            log_error("deserializeJson() failed with code");
            return MessageNoData;
        }

        uint32_t updateID = root["result"][0]["update_id"];
        if (!updateID) return MessageNoData;

        m_lastUpdateId = updateID + 1;
        debugJson(root, Serial);

        if (root["result"][0]["callback_query"]["id"]) {
            // this is a callback query
            message.callbackQueryID   = root["result"][0]["callback_query"]["id"];
            message.chatId            = root["result"][0]["callback_query"]["message"]["chat"]["id"];
            message.sender.id         = root["result"][0]["callback_query"]["from"]["id"];
            message.sender.username   = root["result"][0]["callback_query"]["from"]["username"];
            message.sender.firstName  = root["result"][0]["callback_query"]["from"]["first_name"];
            message.sender.lastName   = root["result"][0]["callback_query"]["from"]["last_name"];
            message.messageID         = root["result"][0]["callback_query"]["message"]["message_id"];
            message.date              = root["result"][0]["callback_query"]["message"]["date"];
            message.chatInstance      = root["result"][0]["callback_query"]["chat_instance"];
            message.callbackQueryData = root["result"][0]["callback_query"]["data"];
            message.text              = root["result"][0]["callback_query"]["message"]["text"].as<String>();
            message.messageType       = MessageQuery;

            // Check if callback function is defined for this button query
            for(uint8_t i=0; i<m_keyboardCount; i++)
                m_keyboards[i]->checkCallback(message);
        }
        else if (root["result"][0]["message"]["message_id"]) {
            // this is a message
            message.messageID        = root["result"][0]["message"]["message_id"];
            message.chatId           = root["result"][0]["message"]["chat"]["id"];
            message.sender.id        = root["result"][0]["message"]["from"]["id"];
            message.sender.username  = root["result"][0]["message"]["from"]["username"];
            message.sender.firstName = root["result"][0]["message"]["from"]["first_name"];
            message.sender.lastName  = root["result"][0]["message"]["from"]["last_name"];
            message.group.title      = root["result"][0]["message"]["chat"]["title"];
            message.date             = root["result"][0]["message"]["date"];

            if (root["result"][0]["message"]["location"]) {
                // this is a location message
                message.location.longitude = root["result"][0]["message"]["location"]["longitude"];
                message.location.latitude = root["result"][0]["message"]["location"]["latitude"];
                message.messageType = MessageLocation;
            }
            else if (root["result"][0]["message"]["contact"]) {
                // this is a contact message
                message.contact.id          = root["result"][0]["message"]["contact"]["user_id"];
                message.contact.firstName   = root["result"][0]["message"]["contact"]["first_name"];
                message.contact.lastName    = root["result"][0]["message"]["contact"]["last_name"];
                message.contact.phoneNumber = root["result"][0]["message"]["contact"]["phone_number"];
                message.contact.vCard       = root["result"][0]["message"]["contact"]["vcard"];
                message.messageType = MessageContact;
            }
            else if (root["result"][0]["message"]["document"]) {
                // this is a document message
                message.document.file_id      = root["result"][0]["message"]["document"]["file_id"];
                message.document.file_name    = root["result"][0]["message"]["document"]["file_name"];
                message.text                  = root["result"][0]["message"]["caption"].as<String>();
                message.document.file_exists  = getFile(message.document);
                message.messageType           = MessageDocument;
            }
            else if (root["result"][0]["message"]["reply_to_message"]) {
                // this is a reply to message
                message.text        = root["result"][0]["message"]["text"].as<String>();
                message.messageType = MessageReply;
            }
            else if (root["result"][0]["message"]["text"]) {
                // this is a text message
                message.text        = root["result"][0]["message"]["text"].as<String>();
                message.messageType = MessageText;
            }
        }
        root.clear();
        return message.messageType;
    }
    return MessageNoData;   // waiting for reply from server
}


// Blocking getMe function (we wait for a reply from Telegram server)
bool AsyncTelegram2::getMe()
{
    // getMe has to be blocking (wait server reply)
    StaticJsonDocument<BUFFER_SMALL> smallDoc;
    if (! sendCommand("getMe", smallDoc, true)) {
        log_error("getMe error");
        return false;
    }
    debugJson(smallDoc, Serial);
    m_botusername = smallDoc["result"]["username"].as<String>();
    return true;
}


bool AsyncTelegram2::getFile(TBDocument &doc)
{
    String cmd((char *)0);
    cmd = "getFile?file_id=";
    cmd += doc.file_id;

    // getFile has to be blocking (wait server reply)
    StaticJsonDocument<BUFFER_MEDIUM> fileDoc;
    if (!sendCommand(cmd.c_str(), fileDoc, true)) {
        log_error("getFile error");
        return false;
    }
    debugJson(fileDoc, Serial);
    doc.file_path = "https://api.telegram.org/file/bot" ;
    doc.file_path += m_token;
    doc.file_path += "/";
    doc.file_path += fileDoc["result"]["file_path"].as<String>();
    doc.file_size  = fileDoc["result"]["file_size"].as<long>();
    return true;
}


bool AsyncTelegram2::noNewMessage() {
    StaticJsonDocument<BUFFER_SMALL> smallDoc;
    smallDoc["allowed_updates"] = "message,callback_query,inline_query";
    smallDoc["offset"] = m_lastUpdateId;
    return sendCommand("getUpdates", smallDoc, true);
}


void AsyncTelegram2::sendMessage(const TBMessage &msg, const char* message, String keyboard)
{
    if (!strlen(message)) return;

    DynamicJsonDocument root(BUFFER_BIG);
    // Backward compatibility
    root["chat_id"] = msg.sender.id != 0 ? msg.sender.id : msg.chatId;
    root["text"] = message;

    if(msg.isMarkdownEnabled)
        root["parse_mode"] = "MarkdownV2";

    if(msg.isHTMLenabled)
        root["parse_mode"] = "HTML";

    if(msg.disable_notification)
        root["disable_notification"] = true;

    if (keyboard.length() || msg.force_reply) {
        StaticJsonDocument<BUFFER_MEDIUM> doc;
        deserializeJson(doc, keyboard);
        JsonObject myKeyb = doc.as<JsonObject>();
        root["reply_markup"] = myKeyb;
        if(msg.force_reply) {
            root["reply_markup"]["selective"] = true,
            root["reply_markup"]["force_reply"] = true;
        }
    }
    root.shrinkToFit();
    debugJson(root, Serial);
    sendCommand("sendMessage", root);
}


void AsyncTelegram2::sendPhotoByUrl(const uint32_t& chat_id,  const String &url, const String &caption)
{
    if (!url.length()) return;

    StaticJsonDocument<BUFFER_SMALL> smallDoc;
    smallDoc["chat_id"] = chat_id;
    smallDoc["photo"] = url;
    smallDoc["caption"] = caption;
    sendCommand("sendPhoto", smallDoc);
    debugJson(smallDoc, Serial);
}


void AsyncTelegram2::sendToChannel(const char* &channel, const String &message, bool silent) {
    if (!message.length()) return;

    StaticJsonDocument<BUFFER_MEDIUM> doc;
    doc["chat_id"] = channel;
    doc["text"] = message;
    if(silent)
        doc["silent"] = true;
    sendCommand("sendMessage", doc);
    debugJson(doc, Serial);
}


void AsyncTelegram2::endQuery(const TBMessage &msg, const char* message, bool alertMode)
{
    if (!strlen(msg.callbackQueryID)) return;

    StaticJsonDocument<BUFFER_SMALL> smallDoc;
    smallDoc["callback_query_id"] =  msg.callbackQueryID;
    if (strlen(message) != 0) {
        smallDoc["text"] = message;
        if (alertMode)
            smallDoc["show_alert"] = true;
        else
            smallDoc["show_alert"] = false;
    }
    sendCommand("answerCallbackQuery", smallDoc);
}


void AsyncTelegram2::removeReplyKeyboard(const TBMessage &msg, const char* message, bool selective)
{
    StaticJsonDocument<BUFFER_SMALL> smallDoc;
    smallDoc["remove_keyboard"] = true;
    if (selective) {
        smallDoc["selective"] = true;
    }
    char command[128];
    serializeJson(smallDoc, command, 128);
    sendMessage(msg, message, command);
}


bool AsyncTelegram2::sendDocument(uint32_t chat_id, const char* command, const char* contentType, const char* binaryPropertyName, Stream* stream, size_t size)
{
    #define BOUNDARY            "----WebKitFormBoundary7MA4YWxkTrZu0gW"
    #define END_BOUNDARY        "\r\n--" BOUNDARY "--\r\n"

    if (telegramClient->connected()) {
        m_waitingReply = true;

        String formData((char *)0);
        formData = "--" BOUNDARY;
        formData += "\r\nContent-disposition: form-data; name=\"chat_id\"\r\n\r\n";
        formData += chat_id;
        formData += "\r\n--" BOUNDARY;
        formData += "\r\nContent-disposition: form-data; name=\"";
        formData += binaryPropertyName;
        formData += "\"; filename=\"";
        formData += "image.jpg";
        formData += "\"\r\nContent-Type: ";
        formData += contentType;
        formData += "\r\n\r\n";
        int contentLength = size + formData.length() + strlen(END_BOUNDARY);

        String request((char *)0);
        request = "POST /bot";
        request += m_token;
        request += "/";
        request += command;
        request += " HTTP/1.1\r\nHost: " TELEGRAM_HOST;
        request += "\r\nContent-Length: ";
        request += contentLength;
        request += "\r\nContent-Type: multipart/form-data; boundary=" BOUNDARY "\r\n";

        // Send POST request to host
        telegramClient->println(request);

        // Body of request
        telegramClient->print(formData);

        // uint32_t t1 = millis();
        // while (stream->available()) {
        //     telegramClient->write((uint8_t)stream->read());
        // }

        uint8_t buff[BLOCK_SIZE];
        uint16_t count = 0;
        while (stream->available()) {
            yield();
            buff[count++] = (uint8_t)stream->read();
            if (count == BLOCK_SIZE ) {
                //log_debug("\nSending binary photo full buffer");
                telegramClient->write((const uint8_t *)buff, BLOCK_SIZE);
                count = 0;
                m_lastmsg_timestamp = millis();
            }
        }
        if (count > 0) {
            //log_debug("\nSending binary photo remaining buffer");
            telegramClient->write((const uint8_t *)buff, count);
        }

        telegramClient->print(END_BOUNDARY);

        // Serial.printf("\nUpload time: %d\n", millis() - t1);
        m_lastmsg_timestamp = millis();
        m_waitingReply = false;
    }
    else {
        Serial.println("\nError: client not connected");
        return false;
    }
    return true;
}