<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<file:__EnableUnstableFeatures(
  'enum_supertyping',
)>>

enum E1 : int {}

enum E2 : int {}

enum F : int {
  use E1, E2;
}
