#pragma once

#include <iostream>
#include <fstream>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <cuda_runtime.h>
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include "adi_tensor_protocol.h"

class TrtLogger : public nvinfer1::ILogger {
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING) {
            std::cout << "[TensorRT] " << msg << std::endl;
        }
    }
};

static TrtLogger gTrtLogger;

struct TrtBinding {
    std::string name;
    bool is_input;
    TensorDataType dtype;
    std::vector<int64_t> dims;
    size_t size_bytes;
};

class TrtModelInstance {
public:
    std::shared_ptr<nvinfer1::IRuntime> runtime;
    std::shared_ptr<nvinfer1::ICudaEngine> engine;
    std::vector<TrtBinding> input_bindings;
    std::vector<TrtBinding> output_bindings;
    size_t total_input_bytes = 0;
    size_t total_output_bytes = 0;

    static TensorDataType trtTypeToAdi(nvinfer1::DataType dt) {
        switch (dt) {
            case nvinfer1::DataType::kFLOAT: return TensorDataType::FP32;
            case nvinfer1::DataType::kHALF:  return TensorDataType::FP16;
            case nvinfer1::DataType::kINT32: return TensorDataType::INT32;
            case nvinfer1::DataType::kINT8:  return TensorDataType::INT8;
            default: return TensorDataType::FP32;
        }
    }

    static size_t getElementSize(TensorDataType dt) {
        switch (dt) {
            case TensorDataType::FP32: return 4;
            case TensorDataType::FP16: return 2;
            case TensorDataType::INT32: return 4;
            case TensorDataType::INT8:  return 1;
            case TensorDataType::FP64: return 8;
            default: return 4;
        }
    }

    void inspectBindings() {
        int num_io = engine->getNbIOTensors();
        for (int i = 0; i < num_io; ++i) {
            const char* name = engine->getIOTensorName(i);
            nvinfer1::TensorIOMode mode = engine->getTensorIOMode(name);
            nvinfer1::DataType dt = engine->getTensorDataType(name);
            nvinfer1::Dims shape = engine->getTensorShape(name);

            TrtBinding binding;
            binding.name = name;
            binding.is_input = (mode == nvinfer1::TensorIOMode::kINPUT);
            binding.dtype = trtTypeToAdi(dt);
            
            size_t count = 1;
            for (int d = 0; d < shape.nbDims; ++d) {
                int64_t dim = shape.d[d] < 0 ? 1 : shape.d[d];
                binding.dims.push_back(dim);
                count *= dim;
            }
            binding.size_bytes = count * getElementSize(binding.dtype);

            if (binding.is_input) {
                input_bindings.push_back(binding);
                total_input_bytes += binding.size_bytes;
            } else {
                output_bindings.push_back(binding);
                total_output_bytes += binding.size_bytes;
            }
        }
    }
};

class ModelManager {
public:
    static ModelManager& instance() {
        static ModelManager mgr;
        return mgr;
    }

    bool loadOnnxModel(uint32_t model_id, const std::string& onnx_path) {
        auto builder = std::unique_ptr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(gTrtLogger));
        const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
        auto network = std::unique_ptr<nvinfer1::INetworkDefinition>(builder->createNetworkV2(explicitBatch));
        auto parser = std::unique_ptr<nvonnxparser::IParser>(nvonnxparser::createParser(*network, gTrtLogger));

        if (!parser->parseFromFile(onnx_path.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kWARNING))) {
            std::cerr << "Failed to parse ONNX model: " << onnx_path << std::endl;
            return false;
        }

        auto config = std::unique_ptr<nvinfer1::IBuilderConfig>(builder->createBuilderConfig());
        config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 1ULL << 30);

        auto serializedModel = std::unique_ptr<nvinfer1::IHostMemory>(builder->buildSerializedNetwork(*network, *config));
        if (!serializedModel) {
            std::cerr << "Failed to build TensorRT serialized network." << std::endl;
            return false;
        }

        auto instance = std::make_shared<TrtModelInstance>();
        instance->runtime = std::shared_ptr<nvinfer1::IRuntime>(nvinfer1::createInferRuntime(gTrtLogger));
        instance->engine = std::shared_ptr<nvinfer1::ICudaEngine>(
            instance->runtime->deserializeCudaEngine(serializedModel->data(), serializedModel->size())
        );

        instance->inspectBindings();
        models_[model_id] = instance;
        std::cout << "[ModelManager] Loaded Model ID: " << model_id << " (" << onnx_path << ")" << std::endl;
        return true;
    }

    std::shared_ptr<TrtModelInstance> getModel(uint32_t model_id) {
        auto it = models_.find(model_id);
        if (it != models_.end()) return it->second;
        return nullptr;
    }

private:
    std::unordered_map<uint32_t, std::shared_ptr<TrtModelInstance>> models_;
};
