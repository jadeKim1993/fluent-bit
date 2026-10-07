set(src
  file.c
  file_rotate.c
  file_uuid.c)

FLB_PLUGIN(out_file "${src}" "")
