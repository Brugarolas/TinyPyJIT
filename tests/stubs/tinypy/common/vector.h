#pragma once
#include <stdbool.h>
#include <string.h>
typedef struct c11_vector { void* data; int length; int capacity; int elem_size; } c11_vector;
void c11_vector__ctor(c11_vector*, int);
void c11_vector__dtor(c11_vector*);
c11_vector c11_vector__copy(const c11_vector*);
void c11_vector__reserve(c11_vector*, int);
void c11_vector__clear(c11_vector*);
void* c11_vector__emplace(c11_vector*);
bool c11_vector__contains(const c11_vector*, void*);
void* c11_vector__submit(c11_vector*, int*);
void c11_vector__swap(c11_vector*, c11_vector*);
int c11_vector__nextcap(c11_vector*);
void c11_vector__extend(c11_vector*, const void*, int);
#define c11__getitem(T,self,index) (((T*)(self)->data)[index])
#define c11__setitem(T,self,index,value) ((T*)(self)->data)[index]=(value)
#define c11__at(T,self,index) ((T*)(self)->data+(index))
#define c11_vector__push(T,self,elem) do { if((self)->length==(self)->capacity)c11_vector__reserve((self),c11_vector__nextcap((self))); ((T*)(self)->data)[(self)->length++]=(elem); } while(0)
#define c11_vector__pop(self) (--(self)->length)
#define c11_vector__back(T,self) (((T*)(self)->data)[(self)->length-1])
#define c11__foreach(T,self,it) for(T* it=(self)->data; it && it!=(T*)(self)->data+(self)->length; ++it)
#ifndef c11_vector__erase
#define c11_vector__erase(T,self,index) do { T* p=(T*)(self)->data+(index); memmove(p,p+1,((self)->length-(index)-1)*sizeof(T)); (self)->length--; } while(0)
#endif
