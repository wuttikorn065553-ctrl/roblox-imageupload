#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <unordered_map>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <curl/curl.h>
#include <ctime>

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize.h"

struct ImageData {
    int width;
    int height;
    std::vector<std::vector<std::vector<uint8_t>>> pixels;
};

struct MemoryBuffer {
    std::vector<unsigned char> data;
};

std::unordered_map<std::string, ImageData> CACHE;

static size_t CurlWriteMemory(
    void* contents,
    size_t size,
    size_t nmemb,
    void* userp
) {
    size_t total = size * nmemb;

    MemoryBuffer* mem = (MemoryBuffer*)userp;

    unsigned char* ptr = (unsigned char*)contents;

    mem->data.insert(
        mem->data.end(),
        ptr,
        ptr + total
    );

    return total;
}

static std::string urlDecode(const std::string& s) {

    std::string out;

    for (size_t i = 0; i < s.size(); ++i) {

        if (s[i] == '%' && i + 2 < s.size()) {

            int v = 0;

            std::istringstream iss(
                s.substr(i + 1, 2)
            );

            if (iss >> std::hex >> v) {

                out.push_back((char)v);

                i += 2;

            } else {

                out.push_back(s[i]);
            }

        } else if (s[i] == '+') {

            out.push_back(' ');

        } else {

            out.push_back(s[i]);
        }
    }

    return out;
}

struct Query {
    std::string imageUrl;
    int resize = 64;
};

static Query parseQuery(const std::string& req) {

    Query q;

    size_t pos = req.find("url=");

    if (pos == std::string::npos) {
        return q;
    }

    size_t end = req.find(" ", pos);

    std::string encoded =
        req.substr(pos + 4, end - (pos + 4));

    q.imageUrl = urlDecode(encoded);

    size_t resizePos = req.find("resize=");

    if (resizePos != std::string::npos) {

        try {

            size_t resizeEnd =
                req.find(" ", resizePos);

            std::string val =
                req.substr(
                    resizePos + 7,
                    resizeEnd - (resizePos + 7)
                );

            q.resize =
                std::max(
                    8,
                    std::min(256, std::stoi(val))
                );

        } catch (...) {}
    }

    return q;
}

static std::string httpJson(
    int code,
    const std::string& body
) {
    std::ostringstream oss;

    oss
        << "HTTP/1.1 "
        << code
        << " OK\r\n"

        << "Content-Type: application/json\r\n"

        << "Access-Control-Allow-Origin: *\r\n"

        << "Connection: close\r\n"

        << "Content-Length: "
        << body.size()
        << "\r\n\r\n"

        << body;

    return oss.str();
}

static std::string sanitizeDiscordUrl(
    std::string url
) {

    size_t media =
        url.find("media.discordapp.net");

    if (media != std::string::npos) {

        url.replace(
            media,
            strlen("media.discordapp.net"),
            "cdn.discordapp.com"
        );
    }

    return url;
}

MemoryBuffer downloadImage(
    const std::string& url
) {

    CURL* curl = curl_easy_init();

    if (!curl) {
        throw std::runtime_error("curl init failed");
    }

    MemoryBuffer mem;

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        url.c_str()
    );

    curl_easy_setopt(
        curl,
        CURLOPT_FOLLOWLOCATION,
        1L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_USERAGENT,
        "Mozilla/5.0"
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        CurlWriteMemory
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &mem
    );

    curl_easy_setopt(
        curl,
        CURLOPT_SSL_VERIFYPEER,
        0L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_SSL_VERIFYHOST,
        0L
    );

    CURLcode res =
        curl_easy_perform(curl);

    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {

        throw std::runtime_error(
            curl_easy_strerror(res)
        );
    }

    if (mem.data.empty()) {

        throw std::runtime_error(
            "empty image"
        );
    }

    std::cout
        << "Downloaded bytes: "
        << mem.data.size()
        << std::endl;

    return mem;
}

ImageData loadImageToMatrix(
    const std::string& originalUrl,
    int resize
) {

    std::string url =
        sanitizeDiscordUrl(originalUrl);

    if (!(url.rfind(
            "https://cdn.discordapp.com/",
            0
        ) == 0)) {

        throw std::runtime_error(
            "Only Discord links allowed"
        );
    }

    MemoryBuffer mem =
        downloadImage(url);

    int w, h, c;

    unsigned char* src =
        stbi_load_from_memory(
            mem.data.data(),
            (int)mem.data.size(),
            &w,
            &h,
            &c,
            3
        );

    if (!src) {

        std::cerr
            << "stbi failed: "
            << stbi_failure_reason()
            << std::endl;

        throw std::runtime_error(
            stbi_failure_reason()
        );
    }

    int target =
        std::max(
            8,
            std::min(256, resize)
        );

    std::vector<unsigned char> pixels(
        target * target * 3
    );

    int ok =
        stbir_resize_uint8(
            src,
            w,
            h,
            0,
            pixels.data(),
            target,
            target,
            0,
            3
        );

    stbi_image_free(src);

    if (!ok) {

        throw std::runtime_error(
            "resize failed"
        );
    }

    ImageData out;

    out.width = target;
    out.height = target;

    out.pixels.resize(
        target,
        std::vector<std::vector<uint8_t>>(
            target,
            std::vector<uint8_t>(3)
        )
    );

    for (int y = 0; y < target; ++y) {

        for (int x = 0; x < target; ++x) {

            out.pixels[y][x][0] =
                pixels[(y * target + x) * 3 + 0];

            out.pixels[y][x][1] =
                pixels[(y * target + x) * 3 + 1];

            out.pixels[y][x][2] =
                pixels[(y * target + x) * 3 + 2];
        }
    }

    return out;
}

std::string toJson(
    const ImageData& img
) {

    std::ostringstream s;

    s << "{\"width\":"
      << img.width
      << ",\"height\":"
      << img.height
      << ",\"pixels\":[";

    for (int y = 0; y < img.height; ++y) {

        s << "[";

        for (int x = 0; x < img.width; ++x) {

            auto& p = img.pixels[y][x];

            s << "["
              << (int)p[0]
              << ","
              << (int)p[1]
              << ","
              << (int)p[2]
              << "]";

            if (x < img.width - 1) {
                s << ",";
            }
        }

        s << "]";

        if (y < img.height - 1) {
            s << ",";
        }
    }

    s << "]}";

    return s.str();
}

int main() {

    int port = 8787;

    char* envPort = getenv("PORT");

    if (envPort) {
        port = std::stoi(envPort);
    }

    int sockfd =
        socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;

    setsockopt(
        sockfd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &opt,
        sizeof(opt)
    );

    sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bind(
        sockfd,
        (sockaddr*)&addr,
        sizeof(addr)
    );

    listen(sockfd, 16);

    std::cout
        << "Server running on port: "
        << port
        << std::endl;

    while (true) {

        int client =
            accept(sockfd, nullptr, nullptr);

        char buf[8192];

        int n =
            read(
                client,
                buf,
                sizeof(buf)-1
            );

        if (n <= 0) {

            close(client);

            continue;
        }

        buf[n] = '\0';

        std::string req(buf);

        try {

            Query q =
                parseQuery(req);

            if (q.imageUrl.empty()) {

                std::string res =
                    httpJson(
                        200,
                        "{\"message\":\"OK\"}"
                    );

                write(
                    client,
                    res.c_str(),
                    res.size()
                );

                close(client);

                continue;
            }

            auto img =
                loadImageToMatrix(
                    q.imageUrl,
                    q.resize
                );

            std::string body =
                toJson(img);

            std::string res =
                httpJson(200, body);

            write(
                client,
                res.c_str(),
                res.size()
            );

        } catch (
            const std::exception& e
        ) {

            std::cerr
                << "ERROR: "
                << e.what()
                << std::endl;

            std::string body =
                std::string("{\"error\":\"")
                + e.what()
                + "\"}";

            std::string res =
                httpJson(500, body);

            write(
                client,
                res.c_str(),
                res.size()
            );
        }

        close(client);
    }
}