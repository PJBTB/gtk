// VERTEX_SHADER:
// coloring.glsl

_OUT_ vec4 final_color;
flat _OUT_ int use_color;

void main() {
  gl_Position = u_projection * u_modelview * vec4(aPosition, 0.0, 1.0);

  vUv = vec2(aUv.x, aUv.y);

  // We use this shader for both plain glyphs (used as mask)
  // and color glpyhs (used as source). The renderer sets
  // aColor to vec4(-1) for color glyhs.
  if (distance(aColor, vec4(-1)) < 0.001)
    use_color = 0;
  else
    use_color = 1;

  final_color = gsk_premultiply(aColor) * u_alpha;
}

// FRAGMENT_SHADER:
// coloring.glsl

_IN_ vec4 final_color;
flat _IN_ int use_color;

void main() {
  vec4 diffuse = GskTexture(u_source, vUv);

  if (use_color == 1)
    gskSetOutputColor(final_color * diffuse.a);
  else
    gskSetOutputColor(diffuse * u_alpha);
}
