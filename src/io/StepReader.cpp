#include "io/StepReader.h"

#include <IFSelect_ReturnStatus.hxx>
#include <Standard_Failure.hxx>
#include <STEPControl_Reader.hxx>

#include <exception>
#include <string>

namespace spo {

namespace {

std::string pathToOcctString(const std::filesystem::path& path) {
    const auto utf8Path = path.u8string();
    return {reinterpret_cast<const char*>(utf8Path.c_str()), utf8Path.size()};
}

}

Result StepReader::canRead(const std::filesystem::path& path) const {
    const auto ext = path.extension().string();
    if (ext == ".step" || ext == ".stp" || ext == ".STEP" || ext == ".STP") {
        return Result::ok();
    }
    return Result::error("需要选择 STEP 或 STP 文件。");
}

StepReadResult StepReader::read(const std::filesystem::path& path) const {
    const auto readable = canRead(path);
    if (!readable.success()) {
        return {readable, {}};
    }

    try {
        STEPControl_Reader reader;
        const auto occtPath = pathToOcctString(path);
        const auto status = reader.ReadFile(occtPath.c_str());
        if (status != IFSelect_RetDone) {
            return {Result::error("OCCT 读取 STEP/STP 文件失败。"), {}};
        }

        const auto roots = reader.NbRootsForTransfer();
        if (roots <= 0) {
            return {Result::error("STEP/STP 文件没有可转换的根对象。"), {}};
        }

        reader.TransferRoots();
        auto shape = reader.OneShape();
        if (shape.IsNull()) {
            return {Result::error("STEP/STP 转换结果为空模型。"), {}};
        }

        return {Result::ok(), ShapeDocument(shape, path)};
    } catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString();
        return {Result::error(std::string("OCCT 读取 STEP/STP 异常：") + (message != nullptr ? message : "未知错误")), {}};
    } catch (const std::exception& error) {
        return {Result::error(std::string("读取 STEP/STP 异常：") + error.what()), {}};
    } catch (...) {
        return {Result::error("读取 STEP/STP 异常：未知错误。"), {}};
    }
}

}
