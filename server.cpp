#include <iostream>
#include <vector>
#include <string>
#include <fstream>
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

std::unordered_map<std::string, ImageData> CACHE;

static std::string urlDecode(const std::string& s) {

    std::string out;
    out.reserve(s.size());

    for (size_t i = 0; i < s.size(); ++i) {

        if (s[i] == '%' && i + 2 < s.size()) {

            int v = 0;

            std::istringstream iss(s.substr(i + 1, 2));

            if (iss >> std::hex >> v) {
                out.push_back(static_cast<char>(v));
                i += 2;
            }

        } else if (s[i] == '+') {

            out.push_back(' ');

        } else {

            out.push_back(s[i]);
        }
    }

    return out;
}

static size_t CurlWriteToFile(
    void* contents,
    size_t size,
    size_t nmemb,
    void* userp
) {
    std::ofstream* ofs = static_cast<std::ofstream*>(userp);

    ofs->write(
        static_cast<char*>(contents),
        size * nmemb
    );

    return size * nmemb;
}

bool downloadImage(
    const std::string& url,
    const std::string& path
) {

    CURL* curl = curl_easy_init();

    if (!curl) {
        return false;
    }

    std::ofstream ofs(path, std::ios::binary);

    if (!ofs.is_open()) {
        curl_easy_cleanup(curl);
        return false;
    }

    struct curl_slist* headers = nullptr;

    headers = curl_slist_append(
        headers,
        "Accept: image/png,image/jpeg,image/webp,*/*"
    );

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToFile);

    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ofs);

    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    curl_easy_setopt(
        curl,
        CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/123.0 Safari/537.36"
    );

    curl_easy_setopt(curl, CURLOPT_REFERER, "https://discord.com/");

    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);

    curl_easy_cleanup(curl);

    ofs.close();

    if (res != CURLE_OK) {

        std::cerr
            << "curl failed: "
            << curl_easy_strerror(res)
            << std::endl;

        return false;
    }

    std::ifstream check(path, std::ios::binary | std::ios::ate);

    if (!check.is_open() || check.tellg() <= 0) {

        std::cerr << "empty image" << std::endl;

        return false;
    }

    return true;
}

struct Query {
    std::string imageUrl;
    int resize = 64;
};

static Query parseQuery(const std::string& req) {

    Query q;

    size_t firstLineEnd = req.find("\r\n");

    std::string firstLine =
        req.substr(0, firstLineEnd);

    size_t qPos = firstLine.find('?');

    if (qPos == std::string::npos) {
        return q;
    }

    std::string query =
        firstLine.substr(qPos + 1);

    size_t spacePos = query.find(' ');

    if (spacePos != std::string::npos) {
        query = query.substr(0, spacePos);
    }

    std::stringstream ss(query);

    std::string pair;

    while (std::getline(ss, pair, '&')) {

        size_t eq = pair.find('=');

        if (eq == std::string::npos) {
            continue;
        }

        std::string key =
            pair.substr(0, eq);

        std::string value =
            pair.substr(eq + 1);

        value = urlDecode(value);

        if (key == "url") {

            q.imageUrl = value;

        } else if (key == "resize") {

            try {

                q.resize =
                    std::max(
                        8,
                        std::min(
                            256,
                            std::stoi(value)
                        )
                    );

            } catch (...) {}
        }
    }

    return q;
}

static std::string sanitizeDiscordUrl(
    std::string url
) {

    size_t pos =
        url.find("media.discordapp.net");

    if (pos != std::string::npos) {

        url.replace(
            pos,
            strlen("media.discordapp.net"),
            "cdn.discordapp.com"
        );
    }

    pos = url.find("format=webp");

    if (pos != std::string::npos) {

        url.replace(
            pos,
            strlen("format=webp"),
            "format=png"
        );
    }

    return url;
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
        ) == 0 ||

          url.rfind(
            "https://media.discordapp.net/",
            0
          ) == 0)) {

        throw std::runtime_error(
            "Only Discord image links allowed"
        );
    }

    std::string tmp =
        "/tmp/img_" +
        std::to_string(std::time(nullptr)) +
        "_" +
        std::to_string(rand()) +
        ".png";

    if (!downloadImage(url, tmp)) {

        throw std::runtime_error(
            "download failed"
        );
    }

    int w, h, c;

    unsigned char* src =
        stbi_load(
            tmp.c_str(),
            &w,
            &h,
            &c,
            3
        );

    if (!src) {

        std::cerr
            << stbi_failure_reason()
            << std::endl;

        remove(tmp.c_str());

        throw std::runtime_error(
            "stbi_load failed"
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

    remove(tmp.c_str());

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

std::string toJson(const ImageData& img) {

    std::ostringstream s;

    s << "{\"width\":"
      << img.width
      << ",\"height\":"
      << img.height
      << ",\"pixels\":[";

    for (int y = 0; y < img.height; ++y) {

        s << "[";

        for (int x = 0; x < img.width; ++x) {

            const auto& p =
                img.pixels[y][x];

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

static std::string makeResponse(
    int code,
    const std::string& body
) {

    std::ostringstream oss;

    oss << "HTTP/1.1 "
        << code;

    if (code == 200) {
        oss << " OK\r\n";
    } else {
        oss << " ERROR\r\n";
    }

    oss << "Content-Type: application/json\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "Content-Length: "
        << body.size()
        << "\r\n\r\n"
        << body;

    return oss.str();
}

int main() {

    int port = 8787;

    char* envPort = getenv("PORT");

    if (envPort) {
        port = std::stoi(envPort);
    }

    int sockfd =
        socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0) {
        return 1;
    }

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

    if (bind(
            sockfd,
            (sockaddr*)&addr,
            sizeof(addr)
        ) < 0) {

        return 1;
    }

    if (listen(sockfd, 16) < 0) {
        return 1;
    }

    std::cout
        << "Server running on port: "
        << port
        << std::endl;

    while (true) {

        int client =
            accept(sockfd, nullptr, nullptr);

        if (client < 0) {
            continue;
        }

        char buf[8192];

        int n =
            read(
                client,
                buf,
                sizeof(buf) - 1
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

                std::string body =
                    "{\"message\":\"use ?url=<url>&resize=64\"}";

                std::string res =
                    makeResponse(200, body);

                write(
                    client,
                    res.c_str(),
                    res.size()
                );

                close(client);

                continue;
            }

            std::string key =
                q.imageUrl +
                "_" +
                std::to_string(q.resize);

            ImageData img;

            if (CACHE.find(key) != CACHE.end()) {

                img = CACHE[key];

            } else {

                img =
                    loadImageToMatrix(
                        q.imageUrl,
                        q.resize
                    );

                CACHE[key] = img;
            }

            std::string body =
                toJson(img);

            std::string res =
                makeResponse(200, body);

            write(
                client,
                res.c_str(),
                res.size()
            );

        } catch (const std::exception& e) {

            std::cerr
                << "ERROR: "
                << e.what()
                << std::endl;

            std::string body =
                std::string("{\"error\":\"")
                + e.what()
                + "\"}";

            std::string res =
                makeResponse(500, body);

            write(
                client,
                res.c_str(),
                res.size()
            );
        }

        close(client);
    }

    return 0;
}