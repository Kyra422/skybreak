#pragma once

#include <glad/glad.h>
#include <cstdio>
#include <vector>

namespace nwii::runtime::gx {

// A failed program must never enter the renderer's shader cache.
inline GLuint CompileGLShaderProgram(const char* vertex_source,
                                     const char* fragment_source) {
  const auto compile_stage = [](GLenum type, const char* source) -> GLuint {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
      std::fprintf(stderr, "[GX shader] Cannot create shader stage.\n");
      return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::vector<char> log(length > 0 ? length : 1, '\0');
    if (length > 0) glGetShaderInfoLog(shader, length, nullptr, log.data());
    std::fprintf(stderr, "[GX shader] %s compilation failed: %s\n",
                 type == GL_VERTEX_SHADER ? "Vertex" : "Fragment", log.data());
    glDeleteShader(shader);
    return 0;
  };
  const GLuint vertex = compile_stage(GL_VERTEX_SHADER, vertex_source);
  if (vertex == 0) return 0;
  const GLuint fragment = compile_stage(GL_FRAGMENT_SHADER, fragment_source);
  if (fragment == 0) {
    glDeleteShader(vertex);
    return 0;
  }
  const GLuint program = glCreateProgram();
  if (program == 0) {
    std::fprintf(stderr, "[GX shader] Cannot create shader program.\n");
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return 0;
  }
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE) return program;
  GLint length = 0;
  glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
  std::vector<char> log(length > 0 ? length : 1, '\0');
  if (length > 0) glGetProgramInfoLog(program, length, nullptr, log.data());
  std::fprintf(stderr, "[GX shader] Program linking failed: %s\n", log.data());
  glDeleteProgram(program);
  return 0;
}

} // namespace nwii::runtime::gx
