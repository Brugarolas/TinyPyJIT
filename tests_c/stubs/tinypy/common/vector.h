#pragma once
#include <stdbool.h>
#include <string.h>
typedef struct c11_vector { void* data; int length; int capacity; int elem_size; } c11_vector;
void c11_vector__ctor(c11_vector*, int);
void c11_vector__dtor(c11_vector*);
void c11_vector__reserve(c11_vector*, int);
void c11_vector__clear(c11_vector*);
void* c11_vector__emplace(c11_vector*);
int c11_vector__nextcap(c11_vector*);
#define c11__getitem(T,self,index) (((T*)(self)->data)[index])
#define c11__setitem(T,self,index,value) ((T*)(self)->data)[index]=(value)
#define c11__at(T,self,index) ((T*)(self)->data+(index))
#define c11_vector__push(T,self,elem) do { if((self)->length==(self)->capacity)c11_vector__reserve((self),c11_vector__nextcap((self))); ((T*)(self)->data)[(self)->length++]=(elem); } while(0)
#define c11__foreach(T,self,it) for(T* it=(self)->data; it && it!=(T*)(self)->data+(self)->length; ++it)
