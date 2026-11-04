#version 450
// Vector-map ID pass: the ring's code, XORed into the target (logic op).
layout(location = 0) flat in uint i_code;
layout(location = 0) out uint o_code;
void main() { o_code = i_code; }
