#include "pocketpy/common/vector.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
void c11_vector__ctor(c11_vector* s,int e){s->data=NULL;s->length=0;s->capacity=0;s->elem_size=e;}
void c11_vector__dtor(c11_vector* s){free(s->data);s->data=NULL;s->length=s->capacity=0;}
c11_vector c11_vector__copy(const c11_vector* s){c11_vector r; c11_vector__ctor(&r,s->elem_size); c11_vector__reserve(&r,s->length); if(s->length) memcpy(r.data,s->data,(size_t)s->length*s->elem_size); r.length=s->length; return r;}
void c11_vector__reserve(c11_vector* s,int c){if(c<4)c=4;if(c<=s->capacity)return;void* p=realloc(s->data,(size_t)c*s->elem_size);if(!p){fprintf(stderr,"oom\n");abort();}s->data=p;s->capacity=c;}
void c11_vector__clear(c11_vector* s){s->length=0;}
void* c11_vector__emplace(c11_vector* s){if(s->length==s->capacity)c11_vector__reserve(s,c11_vector__nextcap(s));void* p=(char*)s->data+(size_t)s->length*s->elem_size;s->length++;return p;}
bool c11_vector__contains(const c11_vector* s,void* e){for(int i=0;i<s->length;i++)if(!memcmp((char*)s->data+(size_t)i*s->elem_size,e,s->elem_size))return true;return false;}
void* c11_vector__submit(c11_vector* s,int* l){void* p=s->data;*l=s->length;s->data=NULL;s->length=s->capacity=0;return p;}
void c11_vector__swap(c11_vector* a,c11_vector* b){c11_vector t=*a;*a=*b;*b=t;}
int c11_vector__nextcap(c11_vector* s){return s->capacity<4?4:s->capacity*2;}
void c11_vector__extend(c11_vector* s,const void* p,int n){if(n<=0)return;int need=s->length+n;if(need>s->capacity)c11_vector__reserve(s,need);memcpy((char*)s->data+(size_t)s->length*s->elem_size,p,(size_t)n*s->elem_size);s->length+=n;}
