<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<file:__EnableUnstableFeatures(
  'enum_supertyping',
)>>

enum E: int as int {
  A = 0;
}

enum F: int {
  use E;
  A = 42;
}
