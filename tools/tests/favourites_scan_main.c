static void show(const char *id, const char *name, void *ctx) { printf("  element id=%s name=%s\n", id, name); (*(int *)ctx)++; }
int main(void)
{
    const char *list = "{\"results\":[{\"id\":\"col-1\",\"libraryId\":\"lib-1\",\"name\":\"Bedtime \\\"best\\\"\",\"description\":null,"
        "\"books\":[{\"id\":\"book-a\",\"media\":{\"id\":\"media-a\",\"metadata\":{\"title\":\"A {tricky} [title], \\\\\",\"name\":\"x\"},\"tags\":[\"id\",\"name\"]}}],\"lastUpdate\":1},"
        "{\"id\":\"col-2\",\"libraryId\":\"lib-1\",\"name\":\"favourites\",\"books\":[],\"createdAt\":2}],\"total\":2,\"limit\":0,\"page\":0,\"sortDesc\":false,\"minified\":false,\"include\":\"\"}";
    const char *one = "{\"id\":\"col-2\",\"libraryId\":\"lib-1\",\"name\":\"Favourites\",\"description\":\"\",\"books\":["
        "{\"id\":\"book-a\",\"ino\":\"1\",\"media\":{\"id\":\"media-a\",\"chapters\":[{\"id\":0,\"title\":\"One\"}],\"audioFiles\":[{\"ino\":\"2\",\"metadata\":{\"filename\":\"a.mp3\"}}]},\"numFiles\":1},"
        "{\"id\":\"book-b\",\"media\":{\"id\":\"media-b\",\"metadata\":{\"series\":[{\"id\":\"ser-1\",\"name\":\"S\"}]}}}],\"lastUpdate\":3}";
    int n = 0; char col[ID_LEN] = "";
    puts("collections list:"); scan_array(list, "results", show, &n);
    scan_array(list, "results", find_collection, col); printf("  favourites collection = '%s' (want col-2), elements %d (want 2)\n", col, n);
    n = 0; puts("one collection:"); scan_array(one, "books", show, &n); printf("  books %d (want 2: book-a, book-b)\n", n);
    n = 0; scan_array("{\"results\":[],\"total\":0}", "results", show, &n); printf("empty: %d (want 0)\n", n);
    return 0;
}
