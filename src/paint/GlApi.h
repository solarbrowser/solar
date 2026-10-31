/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

// OpenGL ES 3 is reached the way FFmpeg is: the program is built against the headers, and the libraries (libEGL, libGLESv2) are opened when the first WebGL context is made, so a
// machine without them has no WebGL and everything else works. The functions are in a table; a name that the library does not have is left null.
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

namespace solar::paint {

// X(Name): the function glName.
#define SOLAR_GL_FUNCTIONS(X) \
  X(ActiveTexture) X(AttachShader) X(BeginQuery) X(BeginTransformFeedback) X(BindAttribLocation) X(BindBuffer) X(BindBufferBase) X(BindBufferRange) X(BindFramebuffer) \
  X(BindRenderbuffer) X(BindSampler) X(BindTexture) X(BindTransformFeedback) X(BindVertexArray) X(BlendColor) X(BlendEquation) X(BlendEquationSeparate) X(BlendFunc) \
  X(BlendFuncSeparate) X(BlitFramebuffer) X(BufferData) X(BufferSubData) X(CheckFramebufferStatus) X(Clear) X(ClearBufferfi) X(ClearBufferfv) X(ClearBufferiv) \
  X(ClearBufferuiv) X(ClearColor) X(ClearDepthf) X(ClearStencil) X(ClientWaitSync) X(ColorMask) X(CompileShader) X(CompressedTexImage2D) X(CompressedTexImage3D) \
  X(CompressedTexSubImage2D) X(CompressedTexSubImage3D) X(CopyBufferSubData) X(CopyTexImage2D) X(CopyTexSubImage2D) X(CopyTexSubImage3D) X(CreateProgram) X(CreateShader) \
  X(CullFace) X(DeleteBuffers) X(DeleteFramebuffers) X(DeleteProgram) X(DeleteQueries) X(DeleteRenderbuffers) X(DeleteSamplers) X(DeleteShader) X(DeleteSync) X(DeleteTextures) \
  X(DeleteTransformFeedbacks) X(DeleteVertexArrays) X(DepthFunc) X(DepthMask) X(DepthRangef) X(DetachShader) X(Disable) X(DisableVertexAttribArray) X(DrawArrays) \
  X(DrawArraysInstanced) X(DrawBuffers) X(DrawElements) X(DrawElementsInstanced) X(DrawRangeElements) X(Enable) X(EnableVertexAttribArray) X(EndQuery) X(EndTransformFeedback) \
  X(FenceSync) X(Finish) X(Flush) X(FramebufferRenderbuffer) X(FramebufferTexture2D) X(FramebufferTextureLayer) X(FrontFace) X(GenBuffers) X(GenFramebuffers) X(GenQueries) \
  X(GenRenderbuffers) X(GenSamplers) X(GenTextures) X(GenTransformFeedbacks) X(GenVertexArrays) X(GenerateMipmap) X(GetActiveAttrib) X(GetActiveUniform) \
  X(GetActiveUniformBlockiv) X(GetActiveUniformBlockName) X(GetActiveUniformsiv) X(GetAttachedShaders) X(GetAttribLocation) X(GetBooleanv) X(GetBufferParameteriv) X(GetError) \
  X(GetFloatv) X(GetFragDataLocation) X(GetFramebufferAttachmentParameteriv) X(GetInteger64v) X(GetIntegeri_v) X(GetIntegerv) X(GetInternalformativ) X(GetProgramInfoLog) \
  X(GetProgramiv) X(GetQueryObjectuiv) X(GetQueryiv) X(GetRenderbufferParameteriv) X(GetSamplerParameterfv) X(GetSamplerParameteriv) X(GetShaderInfoLog) \
  X(GetShaderPrecisionFormat) X(GetShaderSource) X(GetShaderiv) X(GetString) X(GetStringi) X(GetSynciv) X(GetTexParameterfv) X(GetTexParameteriv) X(GetTransformFeedbackVarying) \
  X(GetUniformBlockIndex) X(GetUniformIndices) X(GetUniformLocation) X(GetUniformfv) X(GetUniformiv) X(GetUniformuiv) X(GetVertexAttribIiv) X(GetVertexAttribIuiv) \
  X(GetVertexAttribPointerv) X(GetVertexAttribfv) X(GetVertexAttribiv) X(Hint) X(InvalidateFramebuffer) X(InvalidateSubFramebuffer) X(IsBuffer) X(IsEnabled) X(IsFramebuffer) \
  X(IsProgram) X(IsQuery) X(IsRenderbuffer) X(IsSampler) X(IsShader) X(IsSync) X(IsTexture) X(IsTransformFeedback) X(IsVertexArray) X(LineWidth) X(LinkProgram) X(MapBufferRange) \
  X(PauseTransformFeedback) X(PixelStorei) X(PolygonOffset) X(ReadBuffer) X(ReadPixels) X(RenderbufferStorage) X(RenderbufferStorageMultisample) X(ResumeTransformFeedback) \
  X(SampleCoverage) X(SamplerParameterf) X(SamplerParameteri) X(Scissor) X(ShaderSource) X(StencilFunc) X(StencilFuncSeparate) X(StencilMask) X(StencilMaskSeparate) \
  X(StencilOp) X(StencilOpSeparate) X(TexImage2D) X(TexImage3D) X(TexParameterf) X(TexParameteri) X(TexStorage2D) X(TexStorage3D) X(TexSubImage2D) X(TexSubImage3D) \
  X(TransformFeedbackVaryings) X(Uniform1f) X(Uniform1fv) X(Uniform1i) X(Uniform1iv) X(Uniform1ui) X(Uniform1uiv) X(Uniform2f) X(Uniform2fv) X(Uniform2i) X(Uniform2iv) \
  X(Uniform2ui) X(Uniform2uiv) X(Uniform3f) X(Uniform3fv) X(Uniform3i) X(Uniform3iv) X(Uniform3ui) X(Uniform3uiv) X(Uniform4f) X(Uniform4fv) X(Uniform4i) X(Uniform4iv) \
  X(Uniform4ui) X(Uniform4uiv) X(UniformBlockBinding) X(UniformMatrix2fv) X(UniformMatrix2x3fv) X(UniformMatrix2x4fv) X(UniformMatrix3fv) X(UniformMatrix3x2fv) \
  X(UniformMatrix3x4fv) X(UniformMatrix4fv) X(UniformMatrix4x2fv) X(UniformMatrix4x3fv) X(UnmapBuffer) X(UseProgram) X(ValidateProgram) X(VertexAttrib1f) X(VertexAttrib1fv) \
  X(VertexAttrib2f) X(VertexAttrib2fv) X(VertexAttrib3f) X(VertexAttrib3fv) X(VertexAttrib4f) X(VertexAttrib4fv) X(VertexAttribDivisor) X(VertexAttribI4i) X(VertexAttribI4iv) \
  X(VertexAttribI4ui) X(VertexAttribI4uiv) X(VertexAttribIPointer) X(VertexAttribPointer) X(Viewport) X(WaitSync)

struct GlApi {
#define X(name) decltype(&::gl##name) name = nullptr;
  SOLAR_GL_FUNCTIONS(X)
#undef X
};

// Loads the libraries (once) and makes the contexts: false if there is no EGL or GLES here.
bool LoadGl(GlApi& api);

}  // namespace solar::paint
