// extern class LargeArray;

typedef struct{
  uint64_t nodeData_offset;
  uint64_t edgeIndData_offset;
  uint64_t edgeDst_offset;
  uint64_t edgeData_offset;
  uint64_t outOfLineLocks;
}LC_CSR_root;

POBJ_LAYOUT_BEGIN(raman);
POBJ_LAYOUT_ROOT(raman, LC_CSR_root);
POBJ_LAYOUT_TOID(raman, LargeArray);
POBJ_LAYOUT_END(raman);