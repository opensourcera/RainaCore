#pragma once
struct File {
  operator bool() const { return false; }
  float parseFloat() { return 0; } int parseInt() { return 0; }
  void close() {} bool isDirectory() { return false; }
  template <class... A> void printf(A...) {} template <class T> void print(T) {}
};
struct SpiffsFS { File open(const char*, const char* = "r") { return File(); } };
extern SpiffsFS SPIFFS;
