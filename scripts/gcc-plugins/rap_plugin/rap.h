#ifndef RAP_H_INCLUDED
#define RAP_H_INCLUDED

#include "gcc-common.h"

typedef struct {
	int hash; // will be sign extended to long in reality
} rap_hash_t;

typedef struct {
	unsigned int qual_const:1;
	unsigned int qual_volatile:1;
} rap_hash_flags_t;
extern rap_hash_flags_t imprecise_rap_hash_flags;

extern bool report_fptr_hash;

extern GTY(()) tree rap_hash_type_node;

void siphash24fold(unsigned char *out, const unsigned char *in, unsigned long long inlen, const unsigned char *k);
void rap_calculate_func_hashes(void *event_data, void *data);
rap_hash_t rap_hash_function_type(const_tree fntype, rap_hash_flags_t flags);
rap_hash_t rap_hash_function_decl(const_tree fndecl, rap_hash_flags_t flags);
rap_hash_t rap_hash_function_node_imprecise(cgraph_node_ptr node);
const_tree type_name(const_tree type);
tree create_new_var(tree type, const char *name);

gimple barrier(tree var, bool full);

#if BUILDING_GCC_VERSION >= 4009
opt_pass *make_rap_fptr_pass(void);
#else
struct opt_pass *make_rap_fptr_pass(void);
#endif

#endif
