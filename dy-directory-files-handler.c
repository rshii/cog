/*
 * dy-directory-files-handler.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-directory-files-handler.h"
#include <gio/gio.h>


struct _DyDirectoryFilesHandler {
    GObject parent;
    GFile  *base_path;
};


enum { MIME_PEEK_BYTES = 64 };

static const char s_file_query_attributes[] =
    G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
    G_FILE_ATTRIBUTE_STANDARD_SIZE ","
    G_FILE_ATTRIBUTE_STANDARD_TYPE;

static void* s_request_resolving_index = (void*) 0xCAFECAFE;

G_DEFINE_QUARK (dy-directory-files-handler-data, dy_directory_files_handler);


static void
on_file_read_async_completed (GObject      *source_object,
                              GAsyncResult *result,
                              void         *user_data)
{
    g_autoptr(GFile) file = G_FILE (g_object_ref (source_object));
    g_autoptr(WebKitURISchemeRequest) request = WEBKIT_URI_SCHEME_REQUEST (user_data);

    g_autoptr(GError) error = NULL;
    g_autoptr(GFileInputStream) file_stream =
        g_file_read_finish (file, result, &error);

    if (file_stream) {
        g_autoptr(GInputStream) stream =
            g_buffered_input_stream_new (G_INPUT_STREAM (file_stream));
        g_autoptr(GFileInfo) info =
            G_FILE_INFO (g_object_steal_qdata (source_object, dy_directory_files_handler_quark ()));
        guint64 size =
            g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
        const char *mime_type =
            g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
        webkit_uri_scheme_request_finish (request, stream, size, mime_type);
    } else {
        /*
         * TODO: Generate a nicer error page.
         */
        g_assert_nonnull (error);
        webkit_uri_scheme_request_finish_error (request, error);
    }
}


static void
on_file_query_info_async_completed (GObject      *source_object,
                                    GAsyncResult *result,
                                    void         *user_data)
{
    g_autoptr(GFile) file = G_FILE (g_object_ref (source_object));
    g_autoptr(WebKitURISchemeRequest) request = WEBKIT_URI_SCHEME_REQUEST (user_data);

    g_autoptr(GError) error = NULL;
    g_autoptr(GFileInfo) info = g_file_query_info_finish (file, result, &error);

    if (!info) {
        g_assert_nonnull (error);
        webkit_uri_scheme_request_finish_error (request, error);
        return;
    }

    GFileType type =
        g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_STANDARD_TYPE);

    if (type == G_FILE_TYPE_REGULAR) {
        /*
         * The current file information will be retrieved by 
         * on_file_read_async_completed() to avoid querying again.
         */
        g_object_set_qdata_full (source_object,
                                 dy_directory_files_handler_quark (),
                                 g_steal_pointer (&info),
                                 g_object_unref);
        g_file_read_async (file,
                           G_PRIORITY_DEFAULT,
                           NULL,
                           on_file_read_async_completed,
                           g_steal_pointer (&request));
    } else if (type == G_FILE_TYPE_DIRECTORY) {
        /*
         * If the requst has been marked, it means this function is being
         * called after having previously found a directory. In that case,
         * do not try to resolve "index.html" a second time and produce
         * an error instead.
         */
        if (s_request_resolving_index == g_object_get_qdata (G_OBJECT (request),
                                                             dy_directory_files_handler_quark ()))
        {
            g_autofree char *path = g_file_get_path (file);
            error = g_error_new (dy_directory_files_handler_error_quark (),
                                 DY_DIRECTORY_FILES_HANDLER_ERROR_CANNOT_RESOLVE,
                                 "Path '%s' does not represent a regular file",
                                 path);
            webkit_uri_scheme_request_finish_error (request, error);
        } else {
            /* Mark request as being resolved for its index. */
            g_object_set_qdata (G_OBJECT (request),
                                dy_directory_files_handler_quark (),
                                s_request_resolving_index);
            g_autoptr(GFile) index = g_file_get_child (file, "index.html");
            g_file_query_info_async (index,
                                     s_file_query_attributes,
                                     G_FILE_QUERY_INFO_NONE,
                                     G_PRIORITY_DEFAULT,
                                     NULL,
                                     on_file_query_info_async_completed,
                                     g_steal_pointer (&request));
        }
    } else {
        g_autofree char *path = g_file_get_path (file);
        error = g_error_new (dy_directory_files_handler_error_quark (),
                             DY_DIRECTORY_FILES_HANDLER_ERROR_CANNOT_RESOLVE,
                             "Path '%s' does not represent a regular file or directory",
                             path);
        webkit_uri_scheme_request_finish_error (request, error);
    }
}


static void
dy_directory_files_handler_run (DyRequestHandler       *request_handler,
                                WebKitURISchemeRequest *request)
{
    DyDirectoryFilesHandler *handler = DY_DIRECTORY_FILES_HANDLER (request_handler);

    /*
     * If we get an empty path, redirect to the root resource "/", otherwise
     * subresources cannot load properly as there would be no base URI.
     */
    const char *path = webkit_uri_scheme_request_get_path (request);
    if (path[0] == '\0') {
        g_autofree char *uri =
            g_strconcat (webkit_uri_scheme_request_get_scheme (request), ":/", NULL);
        webkit_web_view_load_uri (webkit_uri_scheme_request_get_web_view (request), uri);
        return;
    }

    /*
     * Skip trailing forward slashes: If an absolute child path would
     * be passed to g_file_get_child() it will ignore the base path.
     */
    while (path[0] == '/')
        ++path;

    g_autoptr(GFile) file = g_file_get_child (handler->base_path, path);
    g_file_query_info_async (file,
                             s_file_query_attributes,
                             G_FILE_QUERY_INFO_NONE,
                             G_PRIORITY_DEFAULT,
                             NULL,
                             on_file_query_info_async_completed,
                             g_object_ref (request));
}


static void
dy_directory_files_handler_iface_init (DyRequestHandlerInterface *iface)
{
    iface->run = dy_directory_files_handler_run;
}


G_DEFINE_TYPE_WITH_CODE (DyDirectoryFilesHandler,
                         dy_directory_files_handler,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (DY_TYPE_REQUEST_HANDLER,
                                                dy_directory_files_handler_iface_init))


static void
dy_directory_files_handler_dispose (GObject *object)
{
    DyDirectoryFilesHandler *handler = DY_DIRECTORY_FILES_HANDLER (object);

    g_clear_object (&handler->base_path);

    G_OBJECT_CLASS (dy_directory_files_handler_parent_class)->dispose (object);
}


static void
dy_directory_files_handler_class_init (DyDirectoryFilesHandlerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = dy_directory_files_handler_dispose;
}


static void
dy_directory_files_handler_init (DyDirectoryFilesHandler *handler)
{
}


G_DEFINE_QUARK (DyDirectoryFilesHandlerError, dy_directory_files_handler_error)


gboolean
dy_directory_files_handler_is_suitable_path (GFile   *file,
                                             GError **error)
{
    g_return_val_if_fail (G_IS_FILE (file), FALSE);

    if (!g_file_is_native (file)) {
        g_autofree char *path = error ? g_file_get_path (file) : NULL;
        g_set_error (error,
                     dy_directory_files_handler_error_quark (),
                     DY_DIRECTORY_FILES_HANDLER_ERROR_PATH_NOT_NATIVE,
                     "Path '%s' is not native",
                     path);
        return FALSE;
    }

    if (g_file_query_file_type (file, G_FILE_QUERY_INFO_NONE, NULL) != G_FILE_TYPE_DIRECTORY) {
        g_autofree char *path = error ? g_file_get_path (file) : NULL;
        g_set_error (error,
                     dy_directory_files_handler_error_quark (),
                     DY_DIRECTORY_FILES_HANDLER_ERROR_PATH_NOT_DIRECTORY,
                     "Path '%s' is not a directory",
                     path);
        return FALSE;
    }

    return TRUE;
}


DyRequestHandler*
dy_directory_files_handler_new (GFile *base_path)
{
    g_return_val_if_fail (dy_directory_files_handler_is_suitable_path (base_path, NULL), NULL);
    DyDirectoryFilesHandler *handler = g_object_new (DY_TYPE_DIRECTORY_FILES_HANDLER, NULL);
    handler->base_path = g_object_ref_sink (base_path);
    return DY_REQUEST_HANDLER (handler);
}