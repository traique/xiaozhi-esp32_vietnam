#include "mcp_server.h"
#include "board.h"
#include "display.h"
#include "settings.h"

#include <esp_log.h>
#include <cJSON.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#define TAG "FinanceStoryTools"
#define FINANCE_HTTP_TIMEOUT_MS 8000

namespace {

struct GoldPriceInfo {
    bool valid = false;
    std::string brand;
    double buy = 0;
    double sell = 0;
    std::string currency;
    std::string updated_at;
    std::string source;
};

struct StockPriceInfo {
    bool valid = false;
    std::string symbol;
    std::string name;
    double price = 0;
    double change = 0;
    double change_percent = 0;
    double volume = 0;
    std::string updated_at;
    std::string source;
};

struct FuelPriceInfo {
    bool valid = false;
    double ron95 = 0;
    double e5 = 0;
    double diesel = 0;
    std::string unit;
    std::string updated_at;
    std::string source;
};

struct FinanceNewsItem {
    std::string title;
    std::string summary;
    std::string url;
    std::string source;
    std::string published_at;
};

std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << std::uppercase << '%' << std::setw(2) << int(c) << std::nouppercase;
        }
    }
    return escaped.str();
}

std::string GetString(cJSON* obj, const char* key) {
    auto* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? std::string(item->valuestring) : std::string();
}

double GetNumber(cJSON* obj, const char* key, double fallback = 0) {
    auto* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

std::string TrimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

class FinanceGateway {
public:
    static FinanceGateway& GetInstance() {
        static FinanceGateway instance;
        return instance;
    }

    bool FetchGoldPrice(GoldPriceInfo& info) {
        info = GoldPriceInfo{};
        std::string body;
        if (!HttpGetJson(BuildUrl("/gold"), body)) {
            return false;
        }

        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            return false;
        }

        bool ok = false;
        do {
            cJSON* data = cJSON_GetObjectItem(root, "data");
            if (!cJSON_IsObject(data)) {
                data = root;
            }

            info.brand = GetString(data, "brand");
            info.buy = GetNumber(data, "buy");
            info.sell = GetNumber(data, "sell");
            info.currency = GetString(data, "currency");
            info.updated_at = GetString(data, "updated_at");
            info.source = GetString(data, "source");

            if (info.brand.empty()) info.brand = "SJC";
            if (info.currency.empty()) info.currency = "VND";
            info.valid = (info.buy > 0 || info.sell > 0);
            ok = info.valid;
        } while (0);

        cJSON_Delete(root);
        return ok;
    }

    bool FetchStockPrice(const std::string& symbol, StockPriceInfo& info) {
        info = StockPriceInfo{};
        if (symbol.empty()) {
            return false;
        }

        std::string body;
        if (!HttpGetJson(BuildUrl("/stock", "symbol=" + UrlEncode(symbol)), body)) {
            return false;
        }

        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            return false;
        }

        bool ok = false;
        do {
            cJSON* data = cJSON_GetObjectItem(root, "data");
            if (!cJSON_IsObject(data)) {
                data = root;
            }

            info.symbol = GetString(data, "symbol");
            info.name = GetString(data, "name");
            info.price = GetNumber(data, "price");
            info.change = GetNumber(data, "change");
            info.change_percent = GetNumber(data, "change_percent");
            info.volume = GetNumber(data, "volume");
            info.updated_at = GetString(data, "updated_at");
            info.source = GetString(data, "source");

            if (info.symbol.empty()) info.symbol = symbol;
            std::transform(info.symbol.begin(), info.symbol.end(), info.symbol.begin(), ::toupper);
            info.valid = info.price > 0;
            ok = info.valid;
        } while (0);

        cJSON_Delete(root);
        return ok;
    }

    bool FetchFuelPrice(FuelPriceInfo& info) {
        info = FuelPriceInfo{};
        std::string body;
        if (!HttpGetJson(BuildUrl("/fuel"), body)) {
            return false;
        }

        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            return false;
        }

        bool ok = false;
        do {
            cJSON* data = cJSON_GetObjectItem(root, "data");
            if (!cJSON_IsObject(data)) {
                data = root;
            }

            info.ron95 = GetNumber(data, "ron95");
            info.e5 = GetNumber(data, "e5");
            info.diesel = GetNumber(data, "diesel");
            info.unit = GetString(data, "unit");
            info.updated_at = GetString(data, "updated_at");
            info.source = GetString(data, "source");

            if (info.unit.empty()) info.unit = "VND/l";
            info.valid = (info.ron95 > 0 || info.e5 > 0 || info.diesel > 0);
            ok = info.valid;
        } while (0);

        cJSON_Delete(root);
        return ok;
    }

    bool FetchFinanceNews(std::vector<FinanceNewsItem>& items, int limit) {
        items.clear();
        if (limit <= 0) limit = 3;

        std::string body;
        if (!HttpGetJson(BuildUrl("/news", "limit=" + std::to_string(limit)), body)) {
            return false;
        }

        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            return false;
        }

        bool ok = false;
        do {
            cJSON* data = cJSON_GetObjectItem(root, "data");
            cJSON* arr = cJSON_IsArray(data) ? data : cJSON_GetObjectItem(root, "items");
            if (!cJSON_IsArray(arr)) arr = data;
            if (!cJSON_IsArray(arr)) break;

            int count = cJSON_GetArraySize(arr);
            for (int i = 0; i < count; ++i) {
                auto* item = cJSON_GetArrayItem(arr, i);
                if (!cJSON_IsObject(item)) continue;

                FinanceNewsItem news_item;
                news_item.title = GetString(item, "title");
                news_item.summary = GetString(item, "summary");
                news_item.url = GetString(item, "url");
                news_item.source = GetString(item, "source");
                news_item.published_at = GetString(item, "published_at");
                if (!news_item.title.empty()) {
                    items.push_back(std::move(news_item));
                }
            }
            ok = !items.empty();
        } while (0);

        cJSON_Delete(root);
        return ok;
    }

    std::string GetConfiguredBaseUrl() const {
        Settings settings("finance", false);
        return TrimTrailingSlash(settings.GetString("base_url", ""));
    }

private:
    FinanceGateway() = default;

    std::string BuildUrl(const std::string& path, const std::string& query = "") const {
        Settings settings("finance", false);
        auto base = TrimTrailingSlash(settings.GetString("base_url", ""));
        auto api_key = settings.GetString("api_key", "");
        if (base.empty()) {
            return "";
        }

        std::string url = base;
        if (!path.empty() && path.front() != '/') {
            url += "/";
        }
        url += path;

        std::string full_query = query;
        if (!api_key.empty()) {
            if (!full_query.empty()) full_query += "&";
            full_query += "api_key=" + UrlEncode(api_key);
        }
        if (!full_query.empty()) {
            url += "?" + full_query;
        }
        return url;
    }

    bool HttpGetJson(const std::string& url, std::string& body) {
        if (url.empty()) {
            ESP_LOGW(TAG, "Finance gateway base_url is empty");
            return false;
        }

        auto& board = Board::GetInstance();
        auto network = board.GetNetwork();
        if (!network) {
            ESP_LOGE(TAG, "Network is not ready");
            return false;
        }

        auto http = network->CreateHttp(FINANCE_HTTP_TIMEOUT_MS);
        if (!http) {
            ESP_LOGE(TAG, "Cannot create HTTP client");
            return false;
        }

        http->SetHeader("Content-Type", "application/json");
        if (!http->Open("GET", url)) {
            ESP_LOGE(TAG, "HTTP open failed: %s", url.c_str());
            return false;
        }

        int status = http->GetStatusCode();
        if (status != 200) {
            ESP_LOGE(TAG, "HTTP status %d for %s", status, url.c_str());
            http->Close();
            return false;
        }

        body = http->ReadAll();
        http->Close();
        return !body.empty();
    }
};

std::string FallbackIfEmpty(const std::string& value, const std::string& fallback) {
    return value.empty() ? fallback : value;
}

std::string FormatMoneyShort(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (value / 1000000.0);
    return oss.str();
}

std::string FormatSigned(double value, int precision = 2) {
    std::ostringstream oss;
    if (value > 0) oss << "+";
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

std::string BuildStoryPrompt(const std::string& topic,
                             const std::string& age_group,
                             const std::string& style,
                             const std::string& lesson,
                             int length_minutes) {
    const std::string safe_topic = FallbackIfEmpty(topic, "một chuyến phiêu lưu tốt bụng trong khu rừng nhỏ");
    const std::string safe_age = FallbackIfEmpty(age_group, "4-8");
    const std::string safe_style = FallbackIfEmpty(style, "nhẹ nhàng, ấm áp, dễ hiểu");
    const std::string safe_lesson = FallbackIfEmpty(lesson, "biết yêu thương, lễ phép và dũng cảm");
    const int safe_minutes = length_minutes <= 0 ? 2 : length_minutes;

    return "Hãy kể một câu chuyện thiếu nhi bằng tiếng Việt, an toàn và dịu dàng cho bé trong khoảng " +
           std::to_string(safe_minutes) + " phút. "
           "Chủ đề: " + safe_topic + ". "
           "Độ tuổi phù hợp: " + safe_age + ". "
           "Phong cách: " + safe_style + ". "
           "Bài học nhẹ nhàng ở cuối truyện: " + safe_lesson + ". "
           "Yêu cầu: câu ngắn, từ dễ hiểu, không bạo lực, không đáng sợ, có mở đầu hấp dẫn, kết thúc ấm áp.";
}

void ShowSystemMessage(const std::string& text) {
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->SetChatMessage("system", text.c_str());
    }
}

void RegisterFinanceAndStoryTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.finance.gold_price",
        "Lấy giá vàng Việt Nam từ finance gateway JSON. Dùng khi người dùng hỏi giá vàng, vàng SJC, vàng hôm nay.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            GoldPriceInfo info;
            auto& gateway = FinanceGateway::GetInstance();
            if (!gateway.FetchGoldPrice(info)) {
                std::string msg = gateway.GetConfiguredBaseUrl().empty()
                    ? "Finance gateway chưa cấu hình base_url trong namespace finance"
                    : "Không lấy được giá vàng";
                return std::string("{\"success\": false, \"message\": \"") + msg + "\"}";
            }

            std::ostringstream preview;
            preview << info.brand << " M:" << FormatMoneyShort(info.buy)
                    << " B:" << FormatMoneyShort(info.sell);
            ShowSystemMessage(preview.str());

            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", true);
            cJSON_AddStringToObject(json, "brand", info.brand.c_str());
            cJSON_AddNumberToObject(json, "buy", info.buy);
            cJSON_AddNumberToObject(json, "sell", info.sell);
            cJSON_AddStringToObject(json, "currency", info.currency.c_str());
            cJSON_AddStringToObject(json, "updated_at", info.updated_at.c_str());
            cJSON_AddStringToObject(json, "source", info.source.c_str());
            return json;
        });

    mcp.AddTool("self.finance.stock_price",
        "Lấy giá cổ phiếu Việt Nam từ finance gateway JSON. Dùng khi người dùng hỏi mã như HPG, FPT, VNM hoặc VNINDEX.",
        PropertyList({
            Property("symbol", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string symbol = properties["symbol"].value<std::string>();
            StockPriceInfo info;
            auto& gateway = FinanceGateway::GetInstance();
            if (!gateway.FetchStockPrice(symbol, info)) {
                std::string msg = gateway.GetConfiguredBaseUrl().empty()
                    ? "Finance gateway chưa cấu hình base_url trong namespace finance"
                    : "Không lấy được giá cổ phiếu";
                return std::string("{\"success\": false, \"message\": \"") + msg + "\"}";
            }

            std::ostringstream preview;
            preview << info.symbol << " " << std::fixed << std::setprecision(2) << info.price
                    << " " << FormatSigned(info.change_percent) << "%";
            ShowSystemMessage(preview.str());

            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", true);
            cJSON_AddStringToObject(json, "symbol", info.symbol.c_str());
            cJSON_AddStringToObject(json, "name", info.name.c_str());
            cJSON_AddNumberToObject(json, "price", info.price);
            cJSON_AddNumberToObject(json, "change", info.change);
            cJSON_AddNumberToObject(json, "change_percent", info.change_percent);
            cJSON_AddNumberToObject(json, "volume", info.volume);
            cJSON_AddStringToObject(json, "updated_at", info.updated_at.c_str());
            cJSON_AddStringToObject(json, "source", info.source.c_str());
            return json;
        });

    mcp.AddTool("self.finance.fuel_price",
        "Lấy giá xăng dầu Việt Nam từ finance gateway JSON. Dùng khi người dùng hỏi giá xăng, RON95, E5 hoặc dầu diesel.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            FuelPriceInfo info;
            auto& gateway = FinanceGateway::GetInstance();
            if (!gateway.FetchFuelPrice(info)) {
                std::string msg = gateway.GetConfiguredBaseUrl().empty()
                    ? "Finance gateway chưa cấu hình base_url trong namespace finance"
                    : "Không lấy được giá xăng";
                return std::string("{\"success\": false, \"message\": \"") + msg + "\"}";
            }

            std::ostringstream preview;
            preview << "R95 " << std::fixed << std::setprecision(0) << info.ron95
                    << " E5 " << info.e5;
            ShowSystemMessage(preview.str());

            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", true);
            cJSON_AddNumberToObject(json, "ron95", info.ron95);
            cJSON_AddNumberToObject(json, "e5", info.e5);
            cJSON_AddNumberToObject(json, "diesel", info.diesel);
            cJSON_AddStringToObject(json, "unit", info.unit.c_str());
            cJSON_AddStringToObject(json, "updated_at", info.updated_at.c_str());
            cJSON_AddStringToObject(json, "source", info.source.c_str());
            return json;
        });

    mcp.AddTool("self.finance.market_news",
        "Lấy tin tức tài chính ngắn gọn từ finance gateway JSON. Dùng khi người dùng hỏi tin tài chính, tin kinh tế, news thị trường.",
        PropertyList({
            Property("limit", kPropertyTypeInteger, 3, 1, 10)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            int limit = properties["limit"].value<int>();
            std::vector<FinanceNewsItem> items;
            auto& gateway = FinanceGateway::GetInstance();
            if (!gateway.FetchFinanceNews(items, limit)) {
                std::string msg = gateway.GetConfiguredBaseUrl().empty()
                    ? "Finance gateway chưa cấu hình base_url trong namespace finance"
                    : "Không lấy được tin tài chính";
                return std::string("{\"success\": false, \"message\": \"") + msg + "\"}";
            }

            ShowSystemMessage(items.front().title);

            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", true);
            cJSON* arr = cJSON_CreateArray();
            for (const auto& item : items) {
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "title", item.title.c_str());
                cJSON_AddStringToObject(o, "summary", item.summary.c_str());
                cJSON_AddStringToObject(o, "url", item.url.c_str());
                cJSON_AddStringToObject(o, "source", item.source.c_str());
                cJSON_AddStringToObject(o, "published_at", item.published_at.c_str());
                cJSON_AddItemToArray(arr, o);
            }
            cJSON_AddItemToObject(root, "items", arr);
            return root;
        });

    mcp.AddTool("self.story.kids_tell",
        "Chuẩn bị prompt kể chuyện cho bé bằng tiếng Việt. Dùng khi người dùng nói kể chuyện cho bé, kể chuyện trước khi ngủ, truyện thiếu nhi.",
        PropertyList({
            Property("topic", kPropertyTypeString, ""),
            Property("age_group", kPropertyTypeString, "4-8"),
            Property("style", kPropertyTypeString, "nhẹ nhàng"),
            Property("lesson", kPropertyTypeString, ""),
            Property("length_minutes", kPropertyTypeInteger, 2, 1, 10)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            const std::string topic = properties["topic"].value<std::string>();
            const std::string age_group = properties["age_group"].value<std::string>();
            const std::string style = properties["style"].value<std::string>();
            const std::string lesson = properties["lesson"].value<std::string>();
            const int length_minutes = properties["length_minutes"].value<int>();

            std::string prompt = BuildStoryPrompt(topic, age_group, style, lesson, length_minutes);
            ShowSystemMessage(FallbackIfEmpty(topic, "Đang chuẩn bị truyện cho bé"));

            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "success", true);
            cJSON_AddStringToObject(root, "title", FallbackIfEmpty(topic, "Truyện cho bé").c_str());
            cJSON_AddStringToObject(root, "opening", "Ngày xửa ngày xưa, có một câu chuyện nhỏ dành cho bé.");
            cJSON_AddStringToObject(root, "story_prompt", prompt.c_str());
            return root;
        });

    ESP_LOGI(TAG, "Finance and kids story tools registered");
}

struct FinanceStoryToolRegistrar {
    FinanceStoryToolRegistrar() {
        RegisterFinanceAndStoryTools();
    }
};

FinanceStoryToolRegistrar g_finance_story_tool_registrar;

}  // namespace
