<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<file:__EnableUnstableFeatures(
  'enum_supertyping',
)>>

enum A : int {}

enum B : int {
  use A;
}

enum C : int as int {}

enum D : int as int {
  use C;
}

enum E : int {
  use C;
}
