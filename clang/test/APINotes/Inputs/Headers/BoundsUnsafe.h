void asdf_counted(int * buf, int len);
void asdf_sized(int * buf, int size);
void asdf_counted_n(int * buf, int len);
void asdf_sized_n(int * buf, int size);
void asdf_ended(int * buf, int * end);

void asdf_sized_mul(int * buf, int size, int count);
void asdf_counted_out(int ** buf, int * len);
void asdf_counted_const(int * buf);
void asdf_counted_nullable(int len, int * _Nullable buf);
void asdf_counted_noescape(int * buf, int len);
void asdf_counted_default_level(int * buf, int len);

void asdf_nterm(char * buf);
