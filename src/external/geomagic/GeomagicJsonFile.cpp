#include "external/geomagic/GeomagicJsonFile.h"

#include <QFile>

#include <system_error>

namespace spo::geomagic_json {

QString pathToQString(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return QString::fromUtf8(
        reinterpret_cast<const char*>(utf8.c_str()),
        static_cast<qsizetype>(utf8.size()));
}

void setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

bool writeDocument(
    const QJsonDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage) {
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            setError(errorMessage, "Could not create JSON output directory: " + error.message());
            return false;
        }
    }

    QFile file(pathToQString(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(errorMessage, "Could not open JSON file for writing.");
        return false;
    }

    if (file.write(document.toJson(QJsonDocument::Indented)) < 0) {
        setError(errorMessage, "Could not write JSON file.");
        return false;
    }

    setError(errorMessage, {});
    return true;
}

}
