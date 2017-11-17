/*
 * SPIRVToMSLConverter.cpp
 *
 * Copyright (c) 2014-2017 The Brenwill Workshop Ltd. (http://www.brenwill.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "SPIRVToMSLConverter.h"
#include "MVKCommonEnvironment.h"
#include "MVKStrings.h"
#include "spirv_msl.hpp"
#include "spirv_glsl.hpp"
#include <spirv-tools/libspirv.h>

using namespace mvk;
using namespace std;


#pragma mark -
#pragma mark SPIRVToMSLConverterContext

// Returns whether the vector contains the value (using a matches(T&) comparison member function). */
template<class T>
bool contains(vector<T>& vec, T& val) {
    for (T& vecVal : vec) { if (vecVal.matches(val)) { return true; } }
    return false;
}

MVK_PUBLIC_SYMBOL bool SPIRVToMSLConverterOptions::matches(SPIRVToMSLConverterOptions& other) {
    if (mslVersion != other.mslVersion) { return false; }
    if (!!shouldFlipVertexY != !!other.shouldFlipVertexY) { return false; }
    if (!!isRenderingPoints != !!other.isRenderingPoints) { return false; }
    return true;
}

MVK_PUBLIC_SYMBOL bool MSLVertexAttribute::matches(MSLVertexAttribute& other) {
    if (location != other.location) { return false; }
    if (mslBuffer != other.mslBuffer) { return false; }
    if (mslOffset != other.mslOffset) { return false; }
    if (mslStride != other.mslStride) { return false; }
    if (!!isPerInstance != !!other.isPerInstance) { return false; }
    return true;
}

MVK_PUBLIC_SYMBOL bool MSLResourceBinding::matches(MSLResourceBinding& other) {
    if (stage != other.stage) { return false; }
    if (descriptorSet != other.descriptorSet) { return false; }
    if (binding != other.binding) { return false; }
    if (mslBuffer != other.mslBuffer) { return false; }
    if (mslTexture != other.mslTexture) { return false; }
    if (mslSampler != other.mslSampler) { return false; }
    return true;
}

// Check them all in case inactive VA's duplicate locations used by active VA's.
MVK_PUBLIC_SYMBOL bool SPIRVToMSLConverterContext::isVertexAttributeLocationUsed(uint32_t location) {
    for (auto& va : vertexAttributes) {
        if ((va.location == location) && va.isUsedByShader) { return true; }
    }
    return false;
}

// Check them all in case inactive VA's duplicate buffers used by active VA's.
MVK_PUBLIC_SYMBOL bool SPIRVToMSLConverterContext::isVertexBufferUsed(uint32_t mslBuffer) {
    for (auto& va : vertexAttributes) {
        if ((va.mslBuffer == mslBuffer) && va.isUsedByShader) { return true; }
    }
    return false;
}

MVK_PUBLIC_SYMBOL bool SPIRVToMSLConverterContext::matches(SPIRVToMSLConverterContext& other) {

    if ( !options.matches(other.options) ) { return false; }

    for (auto& va : vertexAttributes) {
        if (va.isUsedByShader && !contains(other.vertexAttributes, va)) { return false; }
    }

    for (auto& rb : resourceBindings) {
        if (rb.isUsedByShader && !contains(other.resourceBindings, rb)) { return false; }
    }
    
    return true;
}

// Aligns the usage of the destination context to that of the source context.
MVK_PUBLIC_SYMBOL void SPIRVToMSLConverterContext::alignUsageWith(SPIRVToMSLConverterContext& srcContext) {

    for (auto& va : vertexAttributes) {
        va.isUsedByShader = false;
        for (auto& srcVA : srcContext.vertexAttributes) {
            if (va.matches(srcVA)) { va.isUsedByShader = srcVA.isUsedByShader; }
        }
    }

    for (auto& rb : resourceBindings) {
        rb.isUsedByShader = false;
        for (auto& srcRB : srcContext.resourceBindings) {
            if (rb.matches(srcRB)) { rb.isUsedByShader = srcRB.isUsedByShader; }
        }
    }
}


#pragma mark -
#pragma mark SPIRVToMSLConverter

/** Populates content extracted from the SPRI-V compiler. */
void populateFromCompiler(spirv_cross::Compiler& compiler,
                          unordered_map<string, string>& entryPointNameMap,
                          SPIRVLocalSizesByEntryPointName& localSizes);

MVK_PUBLIC_SYMBOL void SPIRVToMSLConverter::setSPIRV(const vector<uint32_t>& spirv) { _spirv = spirv; }

MVK_PUBLIC_SYMBOL void SPIRVToMSLConverter::setSPIRV(const uint32_t* spirvCode, size_t length) {
	_spirv.clear();			// Clear for reuse
	_spirv.reserve(length);
	for (size_t i = 0; i < length; i++) {
		_spirv.push_back(spirvCode[i]);
	}
}

MVK_PUBLIC_SYMBOL const vector<uint32_t>& SPIRVToMSLConverter::getSPIRV() { return _spirv; }

MVK_PUBLIC_SYMBOL bool SPIRVToMSLConverter::convert(SPIRVToMSLConverterContext& context,
													bool shouldLogSPIRV,
													bool shouldLogMSL,
                                                    bool shouldLogGLSL) {
	_wasConverted = true;
	_resultLog.clear();
	_msl.clear();

	if (shouldLogSPIRV) { logSPIRV("Converting"); }

	// Add vertex attributes
	vector<spirv_cross::MSLVertexAttr> vtxAttrs;
	spirv_cross::MSLVertexAttr va;
	for (auto& ctxVA : context.vertexAttributes) {
		va.location = ctxVA.location;
        va.msl_buffer = ctxVA.mslBuffer;
        va.msl_offset = ctxVA.mslOffset;
        va.msl_stride = ctxVA.mslStride;
        va.per_instance = ctxVA.isPerInstance;
        va.used_by_shader = ctxVA.isUsedByShader;
		vtxAttrs.push_back(va);
	}

	// Add resource bindings
	vector<spirv_cross::MSLResourceBinding> resBindings;
	spirv_cross::MSLResourceBinding rb;
	for (auto& ctxRB : context.resourceBindings) {
		rb.desc_set = ctxRB.descriptorSet;
		rb.binding = ctxRB.binding;
		rb.stage = ctxRB.stage;
		rb.msl_buffer = ctxRB.mslBuffer;
		rb.msl_texture = ctxRB.mslTexture;
		rb.msl_sampler = ctxRB.mslSampler;
        rb.used_by_shader = ctxRB.isUsedByShader;
		resBindings.push_back(rb);
	}

    spirv_cross::CompilerMSL mslCompiler(_spirv);

    // Establish the MSL options for the compiler
    // This needs to be done in two steps...for CompilerMSL and its superclass.
    auto mslOpts = mslCompiler.get_options();
    mslOpts.msl_version = context.options.mslVersion;
    mslOpts.enable_point_size_builtin = context.options.isRenderingPoints;
    mslOpts.resolve_specialized_array_lengths = true;
    mslCompiler.set_options(mslOpts);

    auto scOpts = mslCompiler.CompilerGLSL::get_options();
    scOpts.vertex.flip_vert_y = context.options.shouldFlipVertexY;
    mslCompiler.CompilerGLSL::set_options(scOpts);

	try {
		_msl = mslCompiler.compile(&vtxAttrs, &resBindings);
        if (shouldLogMSL) { logSource(_msl, "MSL", "Converted"); }
	} catch (spirv_cross::CompilerError& ex) {
		string errMsg("MSL conversion error: ");
		errMsg += ex.what();
		logError(errMsg.data());
        if (shouldLogMSL) {
            _msl = mslCompiler.get_partial_source();
            logSource(_msl, "MSL", "Partially converted");
        }
	}

    // Populate content extracted from the SPRI-V compiler.
    populateFromCompiler(mslCompiler, _entryPointNameMap, _localSizes);

    // To check GLSL conversion
    if (shouldLogGLSL) {
        spirv_cross::CompilerGLSL glslCompiler(_spirv);
        string glsl;
        try {
            glsl = glslCompiler.compile();
            logSource(glsl, "GLSL", "Estimated original");
        } catch (spirv_cross::CompilerError& ex) {
            string errMsg("Original GLSL extraction error: ");
            errMsg += ex.what();
            logMsg(errMsg.data());
            glsl = glslCompiler.get_partial_source();
            logSource(glsl, "GLSL", "Partially converted");
        }
    }

	// Copy whether the vertex attributes and resource bindings are used by the shader
	uint32_t vaCnt = (uint32_t)vtxAttrs.size();
	for (uint32_t vaIdx = 0; vaIdx < vaCnt; vaIdx++) {
		context.vertexAttributes[vaIdx].isUsedByShader = vtxAttrs[vaIdx].used_by_shader;
	}
	uint32_t rbCnt = (uint32_t)resBindings.size();
	for (uint32_t rbIdx = 0; rbIdx < rbCnt; rbIdx++) {
		context.resourceBindings[rbIdx].isUsedByShader = resBindings[rbIdx].used_by_shader;
	}

	return _wasConverted;
}

/** Appends the message text to the result log. */
void SPIRVToMSLConverter::logMsg(const char* logMsg) {
	string trimMsg = trim(logMsg);
	if ( !trimMsg.empty() ) {
		_resultLog += trimMsg;
		_resultLog += "\n\n";
	}
}

/** Appends the error text to the result log, sets the wasConverted property to false, and returns it. */
bool SPIRVToMSLConverter::logError(const char* errMsg) {
	logMsg(errMsg);
	_wasConverted = false;
	return _wasConverted;
}

/** Appends the SPIR-V to the result log, indicating whether it is being converted or was converted. */
void SPIRVToMSLConverter::logSPIRV(const char* opDesc) {

	string spvLog;
	mvk::logSPIRV(_spirv, spvLog);

	_resultLog += opDesc;
	_resultLog += " SPIR-V:\n";
	_resultLog += spvLog;
	_resultLog += "\nEnd SPIR-V\n\n";
}

/** Validates that the SPIR-V code will disassemble during logging. */
bool SPIRVToMSLConverter::validateSPIRV() {
	if (_spirv.size() < 5) { return false; }
	if (_spirv[0] != spv::MagicNumber) { return false; }
	if (_spirv[4] != 0) { return false; }
	return true;
}

/** Appends the source to the result log, prepending with the operation. */
void SPIRVToMSLConverter::logSource(string& src, const char* srcLang, const char* opDesc) {
    _resultLog += opDesc;
    _resultLog += " ";
    _resultLog += srcLang;
    _resultLog += ":\n";
    _resultLog += src;
    _resultLog += "\nEnd ";
    _resultLog += srcLang;
    _resultLog += "\n\n";
}


#pragma mark Support functions

void populateFromCompiler(spirv_cross::Compiler& compiler,
                          unordered_map<string, string>& entryPointNameMap,
                          SPIRVLocalSizesByEntryPointName& localSizes) {

    uint32_t minDim = 1;
    entryPointNameMap.clear();
    localSizes.clear();
    for (string& epOrigName : compiler.get_entry_points()) {
        auto& ep = compiler.get_entry_point(epOrigName);

        entryPointNameMap[epOrigName] = ep.name;

        auto& wgSize = ep.workgroup_size;
        SPIRVLocalSize spvLS;
        spvLS.width = max(wgSize.x, minDim);
        spvLS.height = max(wgSize.y, minDim);
        spvLS.depth = max(wgSize.z, minDim);
        localSizes[epOrigName] = spvLS;
    }
}

MVK_PUBLIC_SYMBOL void mvk::logSPIRV(vector<uint32_t>& spirv, string& spvLog) {
	if ( !((spirv.size() > 4) &&
		   (spirv[0] == spv::MagicNumber) &&
		   (spirv[4] == 0)) ) { return; }

	uint32_t options = (SPV_BINARY_TO_TEXT_OPTION_INDENT);
	spv_text text;
	spv_diagnostic diagnostic = nullptr;
	spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_0);
	spv_result_t error = spvBinaryToText(context, spirv.data(), spirv.size(), options, &text, &diagnostic);
	spvContextDestroy(context);
	if (error) {
		spvDiagnosticPrint(diagnostic);
		spvDiagnosticDestroy(diagnostic);
		return;
	}
	spvLog.append(text->str, text->length);
	spvTextDestroy(text);
}


