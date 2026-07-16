#pragma once

#include <QJsonDocument>
#include <QString>

#include <filesystem>
#include <string>

namespace spo::geomagic_json {

QString pathToQString(const std::filesystem::path& path);
void setError(std::string* errorMessage, const std::string& message);
bool writeDocument(
    const QJsonDocument& document,
    const std::filesystem::path& path,
    std::string* errorMessage);

}
