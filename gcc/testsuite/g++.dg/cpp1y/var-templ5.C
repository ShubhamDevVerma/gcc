// { dg-do run }
// { dg-options "-std=c++1y" }

template<int A, int B>
  struct S1
  {
    static constexpr int a = A;
    static constexpr int b = B;
  };

template<class T>
  constexpr int var = T::a + T::b;

template<template<int,int> class T, int A>
  constexpr int var2 = var<T<A, A>> + A;

int main ()
{
  return !(
    var2<S1, 40> == 120
  );
}
