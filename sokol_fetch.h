#pragma once
/*
    sokol_fetch.h -- asynchronous data loading/streaming

    Project URL: https://github.com/floooh/sokol

    ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    DON'T USE THIS UNTIL FURTHER NOTICE

    - Don't use sokol_fetch.h for now, the current version assumes that
      it is possible to obtain the content size of a file from the
      HTTP server without downloading the entire file first. Turns out
      that's not possible with vanilla HTTP when the web server serves
      files compressed (in that case the Content-Length is the _compressed_
      size, yet JS/WASM only has access to the uncompressed data).
      Long story short, I need to go back to the drawing board :)
    ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    Do this:
        #define SOKOL_IMPL
    before you include this file in *one* C or C++ file to create the
    implementation.

    Optionally provide the following defines with your own implementations:

    SOKOL_ASSERT(c)             - your own assert macro (default: assert(c))
    SOKOL_MALLOC(s)             - your own malloc function (default: malloc(s))
    SOKOL_FREE(p)               - your own free function (default: free(p))
    SOKOL_LOG(msg)              - your own logging function (default: puts(msg))
    SOKOL_UNREACHABLE()         - a guard macro for unreachable code (default: assert(false))
    SOKOL_API_DECL              - public function declaration prefix (default: extern)
    SOKOL_API_IMPL              - public function implementation prefix (default: -)
    SFETCH_MAX_PATH             - max length of UTF-8 filesystem path / URL (default: 1024 bytes)
    SFETCH_MAX_USERDATA_UINT64  - max size of embedded userdata in number of uint64_t, userdata
                                  will be copied into an 8-byte aligned memory region associated
                                  with each in-flight request, default value is 16 (== 128 bytes)
    SFETCH_MAX_CHANNELS         - max number of IO channels (default is 16, also see sfetch_desc_t.num_channels)

    If sokol_fetch.h is compiled as a DLL, define the following before
    including the declaration or implementation:

    SOKOL_DLL

    On Windows, SOKOL_DLL will define SOKOL_API_DECL as __declspec(dllexport)
    or __declspec(dllimport) as needed.

    NOTE: The following documentation talks a lot about "IO threads". Actual
    threads are only used on platforms where threads are available. The web
    version (emscripten/wasm) doesn't use POSIX-style threads, but instead
    asynchronous Javascript calls chained together by callbacks. The actual
    source code differences between the two approaches have been kept to
    a minimum though.

    FEATURE OVERVIEW
    ================

    - Asynchronously load complete files, or stream files incrementally via
      HTTP (on web platform), or the local file system (on native platforms)

    - Request / response-callback model, user code sends a request
      to initiate a file-load, sokol_fetch.h calls the response callback
      on the same thread when data is ready or user-code needs
      to respond otherwise

    - Not limited to the main-thread or a single thread: A sokol-fetch
      "context" can live on any thread, and multiple contexts
      can operate side-by-side on different threads.

    - Memory management for data buffers is under full control of user code.
      sokol_fetch.h won't allocate memory after it has been setup.

    - Automatic rate-limiting guarantees that only a maximum number of
      requests is processed at any one time, allowing a zero-allocation
      model, where all data is streamed into fixed-size, pre-allocated
      buffers.

    - Active Requests can be paused, continued and cancelled from anywhere
      in the user-thread which sent this request.


    TL;DR EXAMPLE CODE
    ==================
    This is the most-simple example code to load a single data file with a
    known maximum size:

    (1) initialize sokol-fetch with default parameters (but NOTE that the
        default setup parameters provide a safe-but-slow "serialized"
        operation):

        sfetch_setup(&(sfetch_desc_t){ 0 });

    (2) send a fetch-request to load a file from the current directory:

        static uint8_t buf[MAX_FILE_SIZE];

        sfetch_send(&(sfetch_request_t){
            .path = "my_file.txt",
            .callback = response_callback,
            .buffer_ptr = buf,
            .buffer_size = sizeof(buf)
        });

    (3) write a 'response-callback' function, this will be called whenever
        the user-code must respond to state changes of the request
        (most importantly when data has been loaded):

        void response_callback(const sfetch_response_t* response) {
            if (response->fetched) {
                // data has been loaded, and is available via
                // 'buffer_ptr' and 'fetched_size':
                const void* data = response->buffer_ptr;
                uint64_t num_bytes = response->fetched_size;
            }
            if (response->finished) {
                // the 'finished'-flag is the catch-all flag for when the request
                // is finished, no matter if loading was successful of failed,
                // so any cleanup-work should happen here...
                ...
                if (response->failed) {
                    // 'failed' is true in (addition to 'finished') if something
                    // went wrong (file doesn't exist, or less bytes could be
                    // read from the file than expected)
                }
            }
        }

    (4) finally, call sfetch_shutdown() at the end of the application:

        sfetch_shutdown()

    There's many other loading-scenarios, for instance one doesn't have to
    provide a buffer upfront, but instead inspect the file's content-size
    in the response callback, allocate a matching buffer and 'bind' that
    to the request.

    Or it's possible to stream huge files into small fixed-size buffer,
    complete with pausing and continuing the download.

    It's also possible to improve the 'pipeline throughput' by fetching
    multiple files in parallel, but at the same time limit the maximum
    number of requests that can be 'in-flight'.

    For how this all works, please read the following documentation sections :)


    API DOCUMENTATION
    =================

    void sfetch_setup(const sfetch_desc_t* desc)
    --------------------------------------------
    First call sfetch_setup(const sfetch_desc_t*) on any thread before calling
    any other sokol-fetch functions on the same thread.

    sfetch_setup() takes a pointer to an sfetch_desc_t struct with setup
    parameters. Parameters which should use their default values must
    be zero-initialized:

        - max_requests (uint32_t):
            The maximum number of requests that can be alive at any time, the
            default is 128.

        - num_channels (uint32_t):
            The number of "IO channels" used to parallelize and prioritize
            requests, the default is 1.

        - num_lanes (uint32_t):
            The number of "lanes" on a single channel. Each request which is
            currently 'inflight' on a channel occupies one lane until the
            request is finished. This is used for automatic rate-limiting
            (search below for CHANNELS AND LANES for more details). The
            default number of lanes is 1.

    For example, to setup sokol-fetch for max 1024 active requests, 4 channels,
    and 8 lanes per channel in C99:

        sfetch_setup(&(sfetch_desc_t){
            .max_requests = 1024,
            .num_channels = 4,
            .num_lanes = 8
        });

    sfetch_setup() is the only place where sokol-fetch will allocate memory.

    NOTE that the default setup parameters of 1 channel and 1 lane per channel
    has a very poor 'pipeline throughput' since this essentially serializes
    IO requests (a new request will only be processed when the last one has
    finished), and since each request needs at least one roundtrip between
    the user- and IO-thread the throughput will be at most one request per
    frame. Search for LATENCY AND THROUGHPUT below for more information on
    how to increase throughput.

    NOTE that you can call sfetch_setup() on multiple threads, each thread
    will get its own thread-local sokol-fetch instance, which will work
    independently from sokol-fetch instances on other threads.

    void sfetch_shutdown(void)
    --------------------------
    Call sfetch_shutdown() at the end of the application to stop any
    IO threads and free all memory that was allocated in sfetch_setup().

    sfetch_handle_t sfetch_send(const sfetch_request_t* request)
    ------------------------------------------------------------
    Call sfetch_send() to start loading data, the function takes a pointer to an
    sfetch_request_t struct with request parameters and returns a
    sfetch_handle_t identifying the request for later calls. At least
    a path/URL and callback must be provided:

        sfetch_handle_t h = sfetch_send(&(sfetch_request_t){
            .path = "my_file.txt",
            .callback = my_response_callback
        });

    sfetch_send() will return an invalid handle if no request can be allocated
    from the internal pool because all available request items are 'in-flight'.

    The sfetch_request_t struct contains the following parameters (optional
    parameters that are not provided must be zero-initialized):

        - path (const char*, required)
            Pointer to an UTF-8 encoded C string describing the filesystem
            path or HTTP URL. The string will be copied into an internal data
            structure, and passed "as is" (apart from any required
            encoding-conversions) to fopen(), CreateFileW() or
            XMLHttpRequest. The maximum length of the string is defined by
            the SFETCH_MAX_PATH configuration define, the default is 1024 bytes
            including the 0-terminator byte.

        - callback (sfetch_callback_t, required)
            Pointer to a response-callback function which is called when the
            request needs "user code attention". Search below for REQUEST
            STATES AND THE RESPONSE CALLBACK for detailed information about
            handling responses in the response callback.

        - channel (uint32_t, optional)
            Index of the IO channel where the request should be processed.
            Channels are used to parallelize and prioritize requests relative
            to each other. Search below for CHANNELS AND LANES for more
            information. The default channel is 0.

        - buffer_ptr, buffer_size (void*, uint64_t, optional)
            This is a pointer/size pair describing a chunk of memory where
            data will be loaded into. Providing a buffer "upfront" in
            sfetch_request() is optional, and makes sense in the following
            two situations:
                (1) a maximum file size is known for the request, so that
                    is guaranteed that the entire file content will fit
                    into the buffer
                (2) ...or the file should be streamed in small chunks, with the
                    response-callback being called after each chunk to
                    process the partial file data
            Search below for BUFFER MANAGEMENT for more detailed information.

        - user_data_ptr, user_data_size (const void*, uint32_t, both optional)
            user_data_ptr and user_data_size describe an optional POD (plain-old-data)
            associated with the request which will be copied(!) into an internal
            memory block. The maximum default size of this memory block is
            128 bytes (but can be overridden by defining SFETCH_MAX_USERDATA_UINT64
            before including the notification, note that this define is in
            "number of uint64_t", not number of bytes). The user-data
            block is 8-byte aligned, and will be copied via memcpy() (so don't
            put any C++ "smart members" in there).

    NOTE that request handles are strictly thread-local and only unique
    within the thread the handle was created on, and all function calls
    involving a request handle must happen on that same thread.

    bool sfetch_handle_valid(sfetch_handle_t request)
    -------------------------------------------------
    This checks if the provided request handle is valid, and is associated with
    a currently active request. It will return false if:

        - sfetch_send() returned an invalid handle because it couldn't allocate
          a new request from the internal request pool (because they're all
          in flight)
        - the request associated with the handle is no longer alive (because
          it either finished successfully, or the request failed for some
          reason)

    void sfetch_dowork(void)
    ------------------------
    Call sfetch_dowork(void) in regular intervals (for instance once per frame)
    on the same thread as sfetch_setup() to "turn the gears". If you are sending
    requests but never hear back from them in the response callback function, then
    the most likely reason is that you forgot to add the call to sfetch_dowork()
    in the per-frame function.

    sfetch_dowork() roughly performs the following work:

        - any new requests that have been sent with sfetch_send() since the
        last call to sfetch_dowork() will be dispatched to their IO channels,
        permitting that any lanes on that specific channel are available (if
        all lanes are occuped, incoming requests are queued until a lane
        becomes available)

        - a state transition from "user side" to "IO thread side" happens for
        each new request that has been dispatched to a channel.

        - requests dispatched to a channel are either forwarded into that
        channel's worker thread (on native platforms), or cause an HTTP
        request to be sent via an asynchronous XMLHttpRequest (on the web
        platform)

        - for all requests which have finished their current IO operation a
        state transition from "IO thread side" to "user side" happens,
        and the response callback is called

        - requests which are completely finished (either because the entire
        file content has been loaded, or they are in the FAILED state) are
        freed (this just changes their state in the 'request pool', no actual
        memory is freed)

        - requests which are not yet finished are fed back into the
        'incoming' queue of their channel, and the cycle starts again

    void sfetch_cancel(sfetch_handle_t request)
    -------------------------------------------
    This cancels a request in the next sfetch_dowork() call and invokes the
    response callback with (response.failed == true) and (response.finished
    == true) to give user-code a chance to do any cleanup work for the
    request. If sfetch_cancel() is called for a request that is no longer
    alive, nothing bad will happen (the call will simply do nothing).

    void sfetch_pause(sfetch_handle_t request)
    ------------------------------------------
    This pauses an active request in the next sfetch_dowork() call and puts
    it into the PAUSED state. For all requests in PAUSED state, the response
    callback will be called in each call to sfetch_dowork() to give user-code
    a chance to CONTINUE the request (by calling sfetch_continue()). Pausing
    a request makes sense for dynamic rate-limiting in streaming scenarios
    (like video/audio streaming with a fixed number of streaming buffers. As
    soon as all available buffers are filled with download data, downloading
    more data must be prevented to allow video/audio playback to catch up and
    free up empty buffers for new download data.

    void sfetch_continue(sfetch_handle_t request)
    ---------------------------------------------
    Continues a paused request, counterpart to the sfetch_pause() function.

    void sfetch_bind_buffer(sfetch_handle_t request, void* buffer_ptr, uint64_t buffer_size)
    ----------------------------------------------------------------------------------------
    This "binds" a new buffer (pointer/size pair) to an active request. The
    function *must* be called from inside the response-callback, and there
    must not already be another buffer bound.

    Search below for BUFFER MANAGEMENT for more detailed information on
    different buffer-management strategies.

    void* sfetch_unbind_buffer(sfetch_handle_t request)
    ---------------------------------------------------
    This removes the current buffer binding from the request and returns
    a pointer to the previous buffer (useful if the buffer was dynamically
    allocated and it must be freed).

    sfetch_unbind_buffer() *must* be called from inside the response callback.

    The usual code sequence to bind a different buffer in the response
    callback might look like this:

        void response_callback(const sfetch_response_t* response) {
            if (response.fetched) {
                ...
                // switch to a different buffer (in the FETCHED state it is
                // guaranteed that the request has a buffer, otherwise it
                // would have gone into the FAILED state
                void* old_buf_ptr = sfetch_unbind_buffer(response.handle);
                free(old_buf_ptr);
                void* new_buf_ptr = malloc(new_buf_size);
                sfetch_bind_buffer(response.handle, new_buf_ptr, new_buf_size);
            }
            if (response.finished) {
                // unbind and free the currently associated buffer,
                // the buffer pointer could be null if the request has failed
                // NOTE that it is legal to call free() with a nullptr,
                // this happens if the request failed to open its file
                // and never goes into the OPENED state
                void* buf_ptr = sfetch_unbind_buffer(response.handle);
                free(buf_ptr);
            }
        }


    sfetch_desc_t sfetch_desc(void)
    -------------------------------
    sfetch_desc() returns a copy of the sfetch_desc_t struct passed to
    sfetch_setup(), with zero-inititialized values replaced with
    their default values.

    int sfetch_max_userdata_bytes(void)
    -----------------------------------
    This returns the value of the SFETCH_MAX_USERDATA_UINT64 config
    define, but in number of bytes (so SFETCH_MAX_USERDATA_UINT64*8).

    int sfetch_max_path(void)
    -------------------------
    Returns the value of the SFETCH_MAX_PATH config define.


    REQUEST STATES AND THE RESPONSE CALLBACK
    ========================================
    A request goes through a number of states during its lifetime. Depending
    on the current state of a request, it will be 'owned' either by the
    "user-thread" (where the request was sent) or an IO thread.

    You can think of a request as "ping-ponging" between the IO thread and
    user thread, any actual IO work is done on the IO thread, while
    invocations of the response-callback happen on the user-thread.

    All state transitions and callback invocations happen inside the
    sfetch_dowork() function.

    An active request goes through the following states:

    ALLOCATED (user-thread)

        The request has been allocated in sfetch_send() and is
        waiting to be dispatched into its IO channel. When this
        happens, the request will transition into the OPENING state.

    OPENING (IO thread)

        The request is currently being opened on the IO thread. After the
        file has been opened, its file-size will be obtained.

        If a buffer was provided in sfetch_send() the request will
        immediately transition into the FETCHING state and start loading
        data into the buffer.

        If no buffer was provided in sfetch_send(), the request will
        transition into the OPENED state.

        If opening the file failed, the request will transition into
        the FAILED state.

    OPENED (user thread)

        A request will go into the OPENED state after its file has been
        opened successfully, but no buffer was provided to load data
        into.

        In the OPENED state, the response-callback will be called so that the
        user-code can inspect the file-size and provide a matching buffer for
        the request by calling sfetch_bind_buffer().

        After the response callback has been called, and a buffer was provided,
        the request will transition into the FETCHING state.

        If no buffer was provided in the response callback, the request
        will transition into the FAILED state.

        To check in the response-callback if a request is in OPENED state:

            void response_callback(const sfetch_response_t* response) {
                if (response->opened) {
                    // request is in OPENED state, can now inspect the
                    // file's content-size:
                    uint64_t size = response->content_size;
                }
            }

    FETCHING (IO thread)

        While a request is in the FETCHING state, data will be loaded into
        the user-provided buffer.

        If the buffer is full, or the entire file content has been loaded,
        the request will transition into the FETCHED state.

        If something went wrong during loading (less bytes could be
        read than expected), the request will transition into the FAILED
        state.

    FETCHED (user thread)

        The request goes into the FETCHED state either when the request's
        buffer has been completely filled with loaded data, or all file
        data has been loaded.

        The response callback will be called so that the user-code can
        process the loaded data.

        Once all file data has been loaded, the 'finished' flag will be set
        in the response callback's sfetch_response_t argument.

        After the user callback returns, and all file data has been loaded
        (response.finished flag is set) the request has reached its end-of-life
        and will recycled.

        Otherwise, if there's still data to load (because the buffer size
        is smaller than the file's content-size), the request will switch
        back to the FETCHING state to load the next chunk of data.

        Note that it is ok to associate a different buffer or buffer-size
        with the request by calling sfetch_bind_buffer() in the response-callback.

        To check in the response callback for the FETCHED state, and
        independently whether the request is finished:

            void response_callback(const sfetch_response_t* response) {
                if (response->fetched) {
                    // request is in FETCHED state, the loaded data is available
                    // in .buffer_ptr, and the number of bytes that have been
                    // loaded in .fetched_size:
                    const void* data = response->buffer_ptr;
                    const uint64_t num_bytes = response->fetched_size;
                }
                if (response->finished) {
                    // the finished flag is set either when all data
                    // has been loaded, the request has been cancelled,
                    // or the file operation has failed, this is where
                    // any required per-request cleanup work should happen
                }
            }


    FAILED (user thread)

        A request will transition into the FAILED state in the following situations:

            - during OPENING if the file doesn't exist or couldn't be
              opened for other reasons
            - during FETCHING when no buffer is currently associated
              with the request
            - during FETCHING if less than the expected number of bytes
              could be read
            - if a request has been cancelled via sfetch_cancel()

        The response callback will be called once after a request goes
        into the FAILED state, with the response->finished flag set to
        true. This gives the user-code a chance to cleanup any resources
        associated with the request.

        To check for the failed state in the response callback:

            void response_callback(const sfetch_response_t* response) {
                if (response->failed) {
                    // specifically check for the failed state...
                }
                // or you can do a catch-all check via the finished-flag:
                if (response->finished) {
                    if (response->failed) {
                        ...
                    }
                }
            }

    PAUSED (user thread)

        A request will transition into the PAUSED state after user-code
        calls the function sfetch_pause() on the request's handle. Usually
        this happens from within the response-callback in streaming scenarios
        when the data streaming needs to wait for a data decoder (like
        a video/audio player) to catch up.

        While a request is in PAUSED state, the response-callback will be
        called in each sfetch_dowork(), so that the user-code can either
        continue the request by calling sfetch_continue(), or cancel
        the request by calling sfetch_cancel().

        When calling sfetch_continue() on a paused request, the request will
        transition into the FETCHING state. Otherwise if sfetch_cancel() is
        called, the request will switch into the FAILED state.

        To check for the PAUSED state in the response callback:

            void response_callback(const sfetch_response_t* response) {
                if (response->paused) {
                    // we can check here whether the request should
                    // continue to load data:
                    if (should_continue(response->handle)) {
                        sfetch_continue(response->handle);
                    }
                }
            }


    CHANNELS AND LANES
    ==================
    Channels and lanes are (somewhat artificial) concepts to manage
    parallelization, prioritization and rate-limiting.

    Channels can be used to parallelize message processing for better
    'pipeline throughput', and to prioritize messages: user-code could
    reserve one channel for "small and big" streaming downloads,
    another channel for "regular" downloads and yet another high-priority channel
    which would only be used for small files which need to start loading
    immediately.

    Each channel comes with its own IO thread and message queues for pumping
    messages in and out of the thread. The channel where a request is
    processed is selected manually when sending a message:

        sfetch_send(&(sfetch_request_t){
            .path = "my_file.txt",
            .callback = my_reponse_callback,
            .channel = 2
        });

    The number of channels is configured at startup in sfetch_setup() and
    cannot be changed afterwards.

    Channels are completely separate from each other, and a request will
    never "hop" from one channel to another.

    Each channel consists of a fixed number of "lanes" for automatic rate
    limiting:

    When a request is sent to a channel via sfetch_send(), a "free lane" will
    be picked and assigned to the request. The request will occupy this lane
    for its entire life time (also while it is paused). If all lanes of a
    channel are currently occupied, new requests will need to wait until a
    lane becomes unoccupied.

    Since the number of channels and lanes is known upfront, it is guaranteed
    that there will never be more than "num_channels * num_lanes" requests
    in flight at any one time.

    This guarantee eliminates unexpected load- and memory-spikes when
    many requests are sent in very short time, and it allows to pre-allocate
    a fixed number of memory buffers which can be reused for the entire
    "lifetime" of a sokol-fetch context.

    In the most simple scenario - when a maximum file size is known - buffers
    can be statically allocated like this:

        uint8_t buffer[NUM_CHANNELS][NUM_LANES][MAX_FILE_SIZE];

    Then in the user callback pick a buffer by channel and lane,
    and associate it with the request like this:

        void response_callback(const sfetch_response_t* response) {
            if (response->opening) {
                void* ptr = buffer[response->channel][response->lane];
                sfetch_bind_buffer(response->handle, ptr, MAX_FILE_SIZE);
            }
            ...
        }


    BUFFER MANAGEMENT
    =================
    Some additional suggested buffer management strategies:

    Dynamic allocation per request
    ------------------------------
    This might be an option if you don't know a maximum file size upfront,
    or don't have to load a lot of files (I wouldn't recommend dynamic
    allocation per request for loading hundreds or thousands of files):

    (1) don't provide a buffer in the request

        sfetch_send(&(sfetch_request_t){
            .path = "my_file.txt",
            .callback = response_callback
        });

    (2) in the response-callback, allocate a buffer big enough for the entire
        file when the request is in the OPENED state, and free it when the
        finished-flag is set (this makes sure that the buffer is freed both on
        success or failure):

        void response_callback(const sfetch_response_t* response) {
            if (response->opened) {
                // allocate a buffer with the file's content-size
                void* buf = malloc(response->content_size);
                sfetch_bind_buffer(response->handle, buf, response->content_size);
            }
            else if (response->fetched) {
                // file-content has been loaded, do something with the
                // loaded file data...
                const void* ptr = response->buffer_ptr;
                uint64_t num_bytes = response->fetched_size;
                ...
            }

            // if the request is finished (no matter if success or failed),
            // free the buffer
            if (response->finished) {
                if (response->buffer_ptr) {
                    free(response->buffer_ptr);
                }
            }
        }

    Streaming huge files into a small buffer
    ----------------------------------------
    If you want to load a huge file, it may be best to load and process
    the data in small chunks. The response-callback will be called whenever
    the buffer has been completely filled with data, and maybe one last time
    with a partially filled buffer.

    In this example I'm using a dynamically allocated buffer which is freed
    when the request is finished:

    (1) Allocate a buffer that's smaller than the file size, and provide it
        in the request:

        const int buf_size = 64 * 1024;
        void* buf_ptr = malloc(buf_ptr);
        sfetch_send(&(sfetch_request_t){
            .path = "my_huge_file.mpg",
            .callback = response_callback,
            .buffer_ptr = buf_ptr,
            .buffer_size = buf_size
        });

    (2) In the response callback, note that there's no handling for the
        OPENED state. If a buffer was provided upfront, the OPENED state
        will be skipped, and the first state the callback will hear from
        is the FETCHED state (unless something went wrong, then it
        would be FAILED).

        void response_callback(const sfetch_response_t* response) {
            if (response->fetched) {
                // process the next chunk of data:
                const void* ptr = response->buffer_ptr;
                uint64_t num_bytes = response->fetched_size;
                ...
            }

            // don't forget to free the allocated buffer when request is finished:
            if (response->finished) {
                free(response->buffer_ptr);
            }
        }

    Using statically allocated buffers
    ----------------------------------
    Sometimes it's best to not deal with dynamic memory allocation at all
    and use static buffers, you just need to make sure that requests
    from different channels and lanes don't scribble over the same memory.

    This is best done by providing a separate buffer for each channel/lane
    combination:

        #define NUM_CHANNELS (4)
        #define NUM_LANES (8)
        #define MAX_FILE_SIZE (1000000)
        static uint8_t buf[NUM_CHANNELS][NUM_LANES][MAX_FILE_SIZE]
        ...

        // setup sokol-fetch with the right number of channels and lanes:
        sfetch_setup(&(sfetch_desc_t){
            .num_channels = NUM_CHANNELS,
            .num_lanes = NUM_LANES
        };
        ...

        // we can't provide the buffer upfront in sfetch_send(), because
        // we don't know the lane where the request will land on, so binding
        // the buffer needs to happen in the response callback:

        void response_callback(const sfetch_response_t* response) {
            if (response->opened) {
                // select buffer by channel and lane:
                void* buf_ptr = buf[response->channel][response->lane];
                sfetch_bind_buffer(response->handle, buf_ptr, MAX_FILE_SIZE);
            }
            else if (response->fetched) {
                // process the data as usual...
                const void* buf_ptr = response->buffer_ptr;
                uint64_t num_bytes = response->fetched_size;
                ...
            }
            // since the buffer is statically allocated, we don't need to
            // care about freeing any memory...
        }


    Loading a file header first
    ---------------------------
    Let's say you want to load a file format with a fixed-size header block
    first, then create some resource which has its own memory buffer from
    the header attributes and finally load the rest of the file data directly
    into the resource's own memory chunk.

    I'm using per-request dynamically allocated memory again for demonstration
    purposes, but memory management can be quite tricky in this scenario,
    especially for the failure case, so I would *really* recommand using
    a static-buffer scenario here as described above.

    (1) send the request with a buffer of the size of the file header, the
        response callback will then be called as soon as the header is loaded:

        void* buf_ptr = malloc(sizeof(image_header_t));
        sfetch_send(&(sfetch_request_t){
            .path = "my_image_file.img",
            .callback = response_callback,
            .buffer_ptr = buf_ptr,
            .buffer_size = sizeof(image_header_t)
        });

    (2) in the response callback, use the content_offset member to
        differentiate between the image header and actual image data:

        void response_callback(const sfetch_response_t* response) {
            if (response->fetched) {
                if (response->content_offset == 0) {
                    // this is the file header...
                    assert(sizeof(image_header_t) == response->fetched_size);
                    const image_header_t* img_hdr = (const image_header_t*) response->buffer_ptr;

                    // create an image resource...
                    image_t img = image_create(img_hdr);

                    // re-bind the fetch buffer so that the remaining
                    // data is loaded directly into the image's pixel buffer,
                    // NOTE the sequence of unbinding and freeing the old
                    // image-header buffer (since this was dynamically allocated)
                    // and then rebinding the image's pixel buffer:
                    void* header_buffer = sfetch_unbind_buffer(response->handle);
                    free(header_buffer);
                    void* pixel_buffer = image_get_pixel_buffer(img);
                    uint64_t pixel_buffer_size = image_get_pixel_buffer_size(img);
                    sfetch_bind_buffer(response->handle, pixel_buffer, pixel_buffer_size);
                }
                else if (response->content_offset == sizeof(image_header_t)) {
                    // this is where the actual pixel data was loaded
                    // into the image's pixel buffer, we don't need to do
                    // anything here...
                }
            }
            // we still need to handle the failure-case when something went
            // wrong while data was loaded into the dynamically allocated
            // buffer for the image-header... in that case the memory
            // allocated for the header must be freed:
            if (response->failed) {
                // ...is this the header data block? (content_offset is 0)
                void* buf_ptr = sfetch_unbind_buffer(response.handle);
                if (buf_ptr && (response->content_offset == 0)) {
                    free(buf_ptr);
                }
            }
        }


    NOTES ON OPTIMIZING PIPELINE LATENCY AND THROUGHPUT
    ===================================================
    With the default configuration of 1 channel and 1 lane per channel,
    sokol_fetch.h will appear to have a shockingly bad loading performance
    if several files are loaded.

    This has two reasons:

        (1) all parallelization when loading data has been disabled. A new
        request will only be processed, when the last request has finished.

        (2) every invocation of the response-callback adds one frame of latency
        to the request, because callbacks will only be called from within
        sfetch_dowork()

    sokol-fetch takes a few shortcuts to improve step (2) and reduce
    the 'inherent latency' of a request:

        - if a buffer is provided upfront, the response-callback won't be
        called in the OPENED state, but start right with the FETCHED state
        where data has already been loaded into the buffer

        - there is no separate CLOSED state where the callback is invoked
        separately when loading has finished (or the request has failed),
        instead the finished and failed flags will be set as part of
        the last FETCHED invocation

    This means providing a big-enough buffer to fit the entire file is the
    best case, the response callback will only be called once, ideally in
    the next frame (or two calls to sfetch_dowork()).

    If no buffer is provided upfront, one frame of latency is added because
    the response callback needs to be invoked in the OPENED state so that
    the user code can bind a buffer.

    This means the best case for a request without an upfront-provided
    buffer is 2 frames (or 3 calls to sfetch_dowork()).

    That's about what can be done to improve the latency for a single request,
    but the really important step is to improve overall throughput. If you
    need to load thousands of files you don't want that to be completely
    serialized.

    The most important action to increase throughput is to increase the
    number of lanes per channel. This defines how many requests can be
    'in flight' on a single channel at the same time. The guiding decision
    factor for how many lanes you can "afford" is the memory size you want
    to set aside for buffers. Each lane needs its own buffer so that
    the data loaded for one request doesn't scribble over the data
    loaded for another request.

    Here's a simple example of sending 4 requests without upfront buffer
    on a channel with 1, 2 and 4 lanes, each line is one frame:

        1 LANE (8 frames):
            Lane 0:
            -------------
            REQ 0 OPENED
            REQ 0 FETCHED
            REQ 1 OPENED
            REQ 1 FETCHED
            REQ 2 OPENED
            REQ 2 FETCHED
            REQ 3 OPENED
            REQ 3 FETCHED

    Note how the request don't overlap, so they can all use the same buffer.

        2 LANES (4 frames):
            Lane 0:         Lane 1:
            ---------------------------------
            REQ 0 OPENED    REQ 1 OPENED
            REQ 0 FETCHED   REQ 1 FETCHED
            REQ 2 OPENED    REQ 3 OPENED
            REQ 2 FETCHED   REQ 3 FETCHED

    This reduces the overall time to 4 frames, but now you need 2 buffers so
    that requests don't scribble over each other.

        4 LANES (2 frames):
            Lane 0:         Lane 1:         Lane 2:         Lane 3:
            -------------------------------------------------------------
            REQ 0 OPENED    REQ 1 OPENED    REQ 2 OPENED    REQ 3 OPENED
            REQ 0 FETCHED   REQ 1 FETCHED   REQ 2 FETCHED   REQ 3 FETCHED

    Now we're down to the same 'best-case' latency as sending a single
    request.

    Apart from the memory requirements for the streaming buffers (which is
    under your control), you can be generous with the number of channels,
    they don't add any processing overhead.

    The last option for tweaking latency and throughput is channels. Each
    channel works independently from other channels, so while one
    channel is busy working through a large number of requests (or one
    very long streaming download), you can set aside a high-priority channel
    for requests that need to start as soon as possible.

    On platforms with threading support, each channel runs on its own
    thread, but this is mainly an implementation detail to work around
    the blocking traditional file IO functions, not for performance reasons.


    FUTURE PLANS / V2.0 IDEA DUMP
    =============================
    - An optional polling API (as alternative to callback API)
    - Move buffer-management into the API? The "manual management"
      can be quite tricky especially for dynamic allocation scenarios,
      API support for buffer management would simplify cases like
      preventing that requests scribble over each other's buffers, or
      an automatic garbage collection for dynamically allocated buffers,
      or automatically falling back to dynamic allocation if static
      buffers aren't big enough.
    - Pluggable request handlers to load data from other "sources"
      (especially HTTP downloads on native platforms via e.g. libcurl
      would be useful)
    - Allow control over the file offset where data is read from? This
      would need a "manual close" function though.
    - I'm currently not happy how the user-data block is handled, this
      should getting and updating the user-data should be wrapped by
      API functions (similar to bind/unbind buffer)


    LICENSE
    =======
    zlib/libpng license

    Copyright (c) 2019 Andre Weissflog

    This software is provided 'as-is', without any express or implied warranty.
    In no event will the authors be held liable for any damages arising from the
    use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

        1. The origin of this software must not be misrepresented; you must not
        claim that you wrote the original software. If you use this software in a
        product, an acknowledgment in the product documentation would be
        appreciated but is not required.

        2. Altered source versions must be plainly marked as such, and must not
        be misrepresented as being the original software.

        3. This notice may not be removed or altered from any source
        distribution.
*/
#define SOKOL_FETCH_INCLUDED (1)
#include <stdint.h>
#include <stdbool.h>

#ifndef SOKOL_API_DECL
#if defined(_WIN32) && defined(SOKOL_DLL) && defined(SOKOL_IMPL)
#define SOKOL_API_DECL __declspec(dllexport)
#elif defined(_WIN32) && defined(SOKOL_DLL)
#define SOKOL_API_DECL __declspec(dllimport)
#else
#define SOKOL_API_DECL extern
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* configuration values for sfetch_setup() */
typedef struct sfetch_desc_t {
    uint32_t _start_canary;
    uint32_t max_requests;          /* max number of active requests across all channels, default is 128 */
    uint32_t num_channels;          /* number of channels to fetch requests in parallel, default is 1 */
    uint32_t num_lanes;             /* max number of requests active on the same channel, default is 1 */
    uint32_t _end_canary;
} sfetch_desc_t;

/* a request handle to identify an active fetch request, returned by sfetch_send() */
typedef struct sfetch_handle_t { uint32_t id; } sfetch_handle_t;

/* the response struct passed to the response callback */
typedef struct sfetch_response_t {
    sfetch_handle_t handle;         /* request handle this response belongs to */
    bool opened;                    /* true when request is in OPENED state (content_size is available) */
    bool fetched;                   /* true when request is in FETCHED state (fetched data is available) */
    bool paused;                    /* request is currently in paused state */
    bool finished;                  /* this is the last response for this request */
    bool failed;                    /* request has failed (always set together with 'finished') */
    bool cancelled;                 /* request was cancelled (always set together with 'finished') */
    uint32_t channel;               /* the channel which processes this request */
    uint32_t lane;                  /* the lane this request occupies on its channel */
    const char* path;               /* the original filesystem path of the request (FIXME: this is unsafe, wrap in API call?) */
    void* user_data;                /* pointer to read/write user-data area (FIXME: this is unsafe, wrap in API call?) */
    uint64_t content_size;          /* overall file size in bytes */
    uint64_t content_offset;        /* current offset of fetched data chunk in file */
    uint64_t fetched_size;          /* size of fetched data chunk in number of bytes */
    void* buffer_ptr;               /* pointer to buffer with fetched data */
    uint64_t buffer_size;           /* overall buffer size (may be >= than fetched_size!) */
} sfetch_response_t;

/* response callback function signature */
typedef void(*sfetch_callback_t)(const sfetch_response_t*);

/* request parameters passed to sfetch_send() */
typedef struct sfetch_request_t {
    uint32_t _start_canary;
    uint32_t channel;               /* index of channel this request is assigned to (default: 0) */
    const char* path;               /* filesystem path or HTTP URL (required) */
    sfetch_callback_t callback;     /* response callback function pointer (required) */
    void* buffer_ptr;               /* buffer pointer where data will be loaded into (optional) */
    uint64_t buffer_size;           /* buffer size in number of bytes (optional) */
    const void* user_data_ptr;      /* pointer to a POD user-data block which will be memcpy'd(!) (optional) */
    uint32_t user_data_size;        /* size of user-data block (optional) */
    uint32_t _end_canary;
} sfetch_request_t;

/* setup sokol-fetch (can be called on multiple threads) */
SOKOL_API_DECL void sfetch_setup(const sfetch_desc_t* desc);
/* discard a sokol-fetch context */
SOKOL_API_DECL void sfetch_shutdown(void);
/* return true if sokol-fetch has been setup */
SOKOL_API_DECL bool sfetch_valid(void);
/* get the desc struct that was passed to sfetch_setup() */
SOKOL_API_DECL sfetch_desc_t sfetch_desc(void);
/* return the max userdata size in number of bytes (SFETCH_MAX_USERDATA_UINT64 * sizeof(uint64_t)) */
SOKOL_API_DECL int sfetch_max_userdata_bytes(void);
/* return the value of the SFETCH_MAX_PATH implementation config value */
SOKOL_API_DECL int sfetch_max_path(void);

/* send a fetch-request, get handle to request back */
SOKOL_API_DECL sfetch_handle_t sfetch_send(const sfetch_request_t* request);
/* return true if a handle is valid *and* the request is alive */
SOKOL_API_DECL bool sfetch_handle_valid(sfetch_handle_t h);
/* do per-frame work, moves requests into and out of IO threads, and invokes response-callbacks */
SOKOL_API_DECL void sfetch_dowork(void);

/* bind a data buffer to a request (request must not currently have a buffer bound, must be called from response callback */
SOKOL_API_DECL void sfetch_bind_buffer(sfetch_handle_t h, void* buffer_ptr, uint64_t buffer_size);
/* clear the 'buffer binding' of a request, returns previous buffer pointer (can be 0), must be called from response callback */
SOKOL_API_DECL void* sfetch_unbind_buffer(sfetch_handle_t h);
/* cancel a request that's in flight (will call response callback with .cancelled + .finished) */
SOKOL_API_DECL void sfetch_cancel(sfetch_handle_t h);
/* pause a request (will call response callback each frame with .paused) */
SOKOL_API_DECL void sfetch_pause(sfetch_handle_t h);
/* continue a paused request */
SOKOL_API_DECL void sfetch_continue(sfetch_handle_t h);

#ifdef __cplusplus
} /* extern "C" */
#endif

/*--- IMPLEMENTATION ---------------------------------------------------------*/
#ifdef SOKOL_IMPL
#define SOKOL_FETCH_IMPL_INCLUDED (1)
#include <string.h> /* memset, memcpy */

#ifndef SFETCH_MAX_PATH
#define SFETCH_MAX_PATH (1024)
#endif
#ifndef SFETCH_MAX_USERDATA_UINT64
#define SFETCH_MAX_USERDATA_UINT64 (16)
#endif
#ifndef SFETCH_MAX_CHANNELS
#define SFETCH_MAX_CHANNELS (16)
#endif

#ifndef SOKOL_API_IMPL
    #define SOKOL_API_IMPL
#endif
#ifndef SOKOL_DEBUG
    #ifndef NDEBUG
        #define SOKOL_DEBUG (1)
    #endif
#endif
#ifndef SOKOL_ASSERT
    #include <assert.h>
    #define SOKOL_ASSERT(c) assert(c)
#endif
#ifndef SOKOL_MALLOC
    #include <stdlib.h>
    #define SOKOL_MALLOC(s) malloc(s)
    #define SOKOL_FREE(p) free(p)
#endif
#ifndef SOKOL_LOG
    #ifdef SOKOL_DEBUG
        #include <stdio.h>
        #define SOKOL_LOG(s) { SOKOL_ASSERT(s); puts(s); }
    #else
        #define SOKOL_LOG(s)
    #endif
#endif

#ifndef _SOKOL_PRIVATE
    #if defined(__GNUC__)
        #define _SOKOL_PRIVATE __attribute__((unused)) static
    #else
        #define _SOKOL_PRIVATE static
    #endif
#endif

#if defined(__EMSCRIPTEN__)
    #include <emscripten/emscripten.h>
    #define _SFETCH_PLATFORM_EMSCRIPTEN (1)
    #define _SFETCH_PLATFORM_WINDOWS (0)
    #define _SFETCH_PLATFORM_POSIX (0)
    #define _SFETCH_HAS_THREADS (0)
#elif defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #define _SFETCH_PLATFORM_WINDOWS (1)
    #define _SFETCH_PLATFORM_EMSCRIPTEN (0)
    #define _SFETCH_PLATFORM_POSIX (0)
    #define _SFETCH_HAS_THREADS (1)
#else
    #include <pthread.h>
    #include <stdio.h>  /* fopen, fread, fseek, fclose */
    #define _SFETCH_PLATFORM_POSIX (1)
    #define _SFETCH_PLATFORM_EMSCRIPTEN (0)
    #define _SFETCH_PLATFORM_WINDOWS (0)
    #define _SFETCH_HAS_THREADS (1)
#endif

/*=== private type definitions ===============================================*/
typedef struct _sfetch_path_t {
    char buf[SFETCH_MAX_PATH];
} _sfetch_path_t;

typedef struct _sfetch_buffer_t {
    uint8_t* ptr;
    uint64_t size;
} _sfetch_buffer_t;

/* a thread with incoming and outgoing message queue syncing */
#if _SFETCH_PLATFORM_POSIX
typedef struct {
    pthread_t thread;
    pthread_cond_t incoming_cond;
    pthread_mutex_t incoming_mutex;
    pthread_mutex_t outgoing_mutex;
    pthread_mutex_t running_mutex;
    pthread_mutex_t stop_mutex;
    bool stop_requested;
    bool valid;
} _sfetch_thread_t;
#elif _SFETCH_PLATFORM_WINDOWS
typedef struct {
    HANDLE thread;
    HANDLE incoming_event;
    CRITICAL_SECTION incoming_critsec;
    CRITICAL_SECTION outgoing_critsec;
    CRITICAL_SECTION running_critsec;
    CRITICAL_SECTION stop_critsec;
    bool stop_requested;
    bool valid;
} _sfetch_thread_t;
#endif

/* file handle abstraction */
#if _SFETCH_PLATFORM_POSIX
typedef FILE* _sfetch_file_handle_t;
#define _SFETCH_INVALID_FILE_HANDLE (0)
typedef void*(*_sfetch_thread_func_t)(void*);
#elif _SFETCH_PLATFORM_WINDOWS
typedef HANDLE _sfetch_file_handle_t;
#define _SFETCH_INVALID_FILE_HANDLE (INVALID_HANDLE_VALUE)
typedef LPTHREAD_START_ROUTINE _sfetch_thread_func_t;
#endif

/* user-side per-request state */
typedef struct {
    bool pause;                 /* switch item to PAUSED state if true */
    bool cont;                  /* switch item back to FETCHING if true */
    bool cancel;                /* cancel the request, switch into FAILED state */
    /* transfer IO => user thread */
    uint64_t content_size;      /* overall file size */
    uint64_t content_offset;    /* number of bytes fetched so far */
    uint64_t fetched_size;      /* size of last fetched chunk */
    bool finished;
    /* user thread only */
    uint32_t user_data_size;
    uint64_t user_data[SFETCH_MAX_USERDATA_UINT64];
} _sfetch_item_user_t;

/* thread-side per-request state */
typedef struct {
    /* transfer IO => user thread */
    uint64_t content_size;
    uint64_t content_offset;
    uint64_t fetched_size;
    bool failed;
    bool finished;
    /* IO thread only */
    #if !_SFETCH_PLATFORM_EMSCRIPTEN
    _sfetch_file_handle_t file_handle;
    #endif
} _sfetch_item_thread_t;

/* a request goes through the following states, ping-ponging between IO and user thread */
typedef enum _sfetch_state_t {
    _SFETCH_STATE_INITIAL,      /* internal: request has just been initialized */
    _SFETCH_STATE_ALLOCATED,    /* internal: request has been allocated from internal pool */
    _SFETCH_STATE_OPENING,      /* IO thread: waiting to be opened */
    _SFETCH_STATE_OPENED,       /* user thread: follow state of OPENING if no buffer was provided */
    _SFETCH_STATE_FETCHING,     /* IO thread: waiting for data to be fetched */
    _SFETCH_STATE_FETCHED,      /* user thread: fetched data available */
    _SFETCH_STATE_PAUSED,       /* user thread: request has been paused via sfetch_pause() */
    _SFETCH_STATE_FAILED,       /* user thread: follow state of OPENING or FETCHING if something went wrong */
} _sfetch_state_t;

/* an internal request item */
#define _SFETCH_INVALID_LANE (0xFFFFFFFF)
typedef struct {
    sfetch_handle_t handle;
    _sfetch_state_t state;
    uint32_t channel;
    uint32_t lane;
    sfetch_callback_t callback;
    _sfetch_buffer_t buffer;

    /* updated by IO-thread, off-limits to user thread */
    _sfetch_item_thread_t thread;

    /* accessible by user-thread, off-limits to IO thread */
    _sfetch_item_user_t user;

    /* big stuff at the end */
    _sfetch_path_t path;
} _sfetch_item_t;

/* a pool of internal per-request items */
typedef struct {
    uint32_t size;
    uint32_t free_top;
    _sfetch_item_t* items;
    uint32_t* free_slots;
    uint32_t* gen_ctrs;
    bool valid;
} _sfetch_pool_t;

/* a ringbuffer for pool-slot ids */
typedef struct {
    uint32_t head;
    uint32_t tail;
    uint32_t num;
    uint32_t* buf;
} _sfetch_ring_t;

/* an IO channel with its own IO thread */
struct _sfetch_t;
typedef struct {
    struct _sfetch_t* ctx;  /* back-pointer to thread-local _sfetch state pointer,
                               since this isn't accessible from the IO threads */
    _sfetch_ring_t free_lanes;
    _sfetch_ring_t user_sent;
    _sfetch_ring_t user_incoming;
    _sfetch_ring_t user_outgoing;
    #if _SFETCH_HAS_THREADS
    _sfetch_ring_t thread_incoming;
    _sfetch_ring_t thread_outgoing;
    _sfetch_thread_t thread;
    #endif
    void (*request_handler)(struct _sfetch_t* ctx, uint32_t slot_id);
    bool valid;
} _sfetch_channel_t;

/* the sfetch global state */
typedef struct _sfetch_t {
    bool setup;
    bool valid;
    bool in_callback;
    sfetch_desc_t desc;
    _sfetch_pool_t pool;
    _sfetch_channel_t chn[SFETCH_MAX_CHANNELS];
} _sfetch_t;
#if _SFETCH_HAS_THREADS
#if defined(_MSC_VER)
static __declspec(thread) _sfetch_t* _sfetch;
#else
static __thread _sfetch_t* _sfetch;
#endif
#else
static _sfetch_t* _sfetch;
#endif

/*=== general helper functions and macros =====================================*/
#define _sfetch_def(val, def) (((val) == 0) ? (def) : (val))

_SOKOL_PRIVATE _sfetch_t* _sfetch_ctx(void) {
    return _sfetch;
}

_SOKOL_PRIVATE void _sfetch_path_copy(_sfetch_path_t* dst, const char* src) {
    SOKOL_ASSERT(dst);
    if (src && (strlen(src) < SFETCH_MAX_PATH)) {
        #if defined(_MSC_VER)
        strncpy_s(dst->buf, SFETCH_MAX_PATH, src, (SFETCH_MAX_PATH-1));
        #else
        strncpy(dst->buf, src, SFETCH_MAX_PATH);
        #endif
        dst->buf[SFETCH_MAX_PATH-1] = 0;
    }
    else {
        memset(dst->buf, 0, SFETCH_MAX_PATH);
    }
}

_SOKOL_PRIVATE _sfetch_path_t _sfetch_path_make(const char* str) {
    _sfetch_path_t res;
    _sfetch_path_copy(&res, str);
    return res;
}

_SOKOL_PRIVATE uint32_t _sfetch_make_id(uint32_t index, uint32_t gen_ctr) {
    return (gen_ctr<<16) | (index & 0xFFFF);
}

_SOKOL_PRIVATE sfetch_handle_t _sfetch_make_handle(uint32_t slot_id) {
    sfetch_handle_t h;
    h.id = slot_id;
    return h;
}

_SOKOL_PRIVATE uint32_t _sfetch_slot_index(uint32_t slot_id) {
    return slot_id & 0xFFFF;
}

/*=== a circular message queue ===============================================*/
_SOKOL_PRIVATE uint32_t _sfetch_ring_wrap(const _sfetch_ring_t* rb, uint32_t i) {
    return i % rb->num;
}

_SOKOL_PRIVATE void _sfetch_ring_discard(_sfetch_ring_t* rb) {
    SOKOL_ASSERT(rb);
    if (rb->buf) {
        SOKOL_FREE(rb->buf);
        rb->buf = 0;
    }
    rb->head = 0;
    rb->tail = 0;
    rb->num = 0;
}

_SOKOL_PRIVATE bool _sfetch_ring_init(_sfetch_ring_t* rb, uint32_t num_slots) {
    SOKOL_ASSERT(rb && (num_slots > 0));
    SOKOL_ASSERT(0 == rb->buf);
    rb->head = 0;
    rb->tail = 0;
    /* one slot reserved to detect full vs empty */
    rb->num = num_slots + 1;
    const size_t queue_size = rb->num * sizeof(sfetch_handle_t);
    rb->buf = (uint32_t*) SOKOL_MALLOC(queue_size);
    if (rb->buf) {
        memset(rb->buf, 0, queue_size);
        return true;
    }
    else {
        _sfetch_ring_discard(rb);
        return false;
    }
}

_SOKOL_PRIVATE bool _sfetch_ring_full(const _sfetch_ring_t* rb) {
    SOKOL_ASSERT(rb && rb->buf);
    return _sfetch_ring_wrap(rb, rb->head + 1) == rb->tail;
}

_SOKOL_PRIVATE bool _sfetch_ring_empty(const _sfetch_ring_t* rb) {
    SOKOL_ASSERT(rb && rb->buf);
    return rb->head == rb->tail;
}

_SOKOL_PRIVATE uint32_t _sfetch_ring_count(const _sfetch_ring_t* rb) {
    SOKOL_ASSERT(rb && rb->buf);
    uint32_t count;
    if (rb->head >= rb->tail) {
        count = rb->head - rb->tail;
    }
    else {
        count = (rb->head + rb->num) - rb->tail;
    }
    SOKOL_ASSERT(count < rb->num);
    return count;
}

_SOKOL_PRIVATE void _sfetch_ring_enqueue(_sfetch_ring_t* rb, uint32_t slot_id) {
    SOKOL_ASSERT(rb && rb->buf);
    SOKOL_ASSERT(!_sfetch_ring_full(rb));
    SOKOL_ASSERT(rb->head < rb->num);
    rb->buf[rb->head] = slot_id;
    rb->head = _sfetch_ring_wrap(rb, rb->head + 1);
}

_SOKOL_PRIVATE uint32_t _sfetch_ring_dequeue(_sfetch_ring_t* rb) {
    SOKOL_ASSERT(rb && rb->buf);
    SOKOL_ASSERT(!_sfetch_ring_empty(rb));
    SOKOL_ASSERT(rb->tail < rb->num);
    uint32_t slot_id = rb->buf[rb->tail];
    rb->tail = _sfetch_ring_wrap(rb, rb->tail + 1);
    return slot_id;
}

_SOKOL_PRIVATE uint32_t _sfetch_ring_peek(const _sfetch_ring_t* rb, uint32_t index) {
    SOKOL_ASSERT(rb && rb->buf);
    SOKOL_ASSERT(!_sfetch_ring_empty(rb));
    SOKOL_ASSERT(index < _sfetch_ring_count(rb));
    uint32_t rb_index = _sfetch_ring_wrap(rb, rb->tail + index);
    return rb->buf[rb_index];
}

/*=== request pool implementation ============================================*/
_SOKOL_PRIVATE void _sfetch_item_init(_sfetch_item_t* item, uint32_t slot_id, const sfetch_request_t* request) {
    SOKOL_ASSERT(item && (0 == item->handle.id));
    SOKOL_ASSERT(request && request->path);
    item->handle.id = slot_id;
    item->state = _SFETCH_STATE_INITIAL;
    item->channel = request->channel;
    item->lane = _SFETCH_INVALID_LANE;
    item->callback = request->callback;
    item->buffer.ptr = (uint8_t*) request->buffer_ptr;
    item->buffer.size = request->buffer_size;
    item->path = _sfetch_path_make(request->path);
    #if !_SFETCH_PLATFORM_EMSCRIPTEN
    item->thread.file_handle = _SFETCH_INVALID_FILE_HANDLE;
    #endif
    if (request->user_data_ptr &&
        (request->user_data_size > 0) &&
        (request->user_data_size <= (SFETCH_MAX_USERDATA_UINT64*8)))
    {
        item->user.user_data_size = request->user_data_size;
        memcpy(item->user.user_data, request->user_data_ptr, request->user_data_size);
    }
}

_SOKOL_PRIVATE void _sfetch_item_discard(_sfetch_item_t* item) {
    SOKOL_ASSERT(item && (0 != item->handle.id));
    memset(item, 0, sizeof(_sfetch_item_t));
}

_SOKOL_PRIVATE void _sfetch_pool_discard(_sfetch_pool_t* pool) {
    SOKOL_ASSERT(pool);
    if (pool->free_slots) {
        SOKOL_FREE(pool->free_slots);
        pool->free_slots = 0;
    }
    if (pool->gen_ctrs) {
        SOKOL_FREE(pool->gen_ctrs);
        pool->gen_ctrs = 0;
    }
    if (pool->items) {
        SOKOL_FREE(pool->items);
        pool->items = 0;
    }
    pool->size = 0;
    pool->free_top = 0;
    pool->valid = false;
}

_SOKOL_PRIVATE bool _sfetch_pool_init(_sfetch_pool_t* pool, uint32_t num_items) {
    SOKOL_ASSERT(pool && (num_items > 0) && (num_items < ((1<<16)-1)));
    SOKOL_ASSERT(0 == pool->items);
    /* NOTE: item slot 0 is reserved for the special "invalid" item index 0*/
    pool->size = num_items + 1;
    pool->free_top = 0;
    const size_t items_size = pool->size * sizeof(_sfetch_item_t);
    pool->items = (_sfetch_item_t*) SOKOL_MALLOC(items_size);
    /* generation counters indexable by pool slot index, slot 0 is reserved */
    const size_t gen_ctrs_size = sizeof(uint32_t) * pool->size;
    pool->gen_ctrs = (uint32_t*) SOKOL_MALLOC(gen_ctrs_size);
    SOKOL_ASSERT(pool->gen_ctrs);
    /* NOTE: it's not a bug to only reserve num_items here */
    const size_t free_slots_size = num_items * sizeof(int);
    pool->free_slots = (uint32_t*) SOKOL_MALLOC(free_slots_size);
    if (pool->items && pool->free_slots) {
        memset(pool->items, 0, items_size);
        memset(pool->gen_ctrs, 0, gen_ctrs_size);
        /* never allocate the 0-th item, this is the reserved 'invalid item' */
        for (uint32_t i = pool->size - 1; i >= 1; i--) {
            pool->free_slots[pool->free_top++] = i;
        }
        pool->valid = true;
    }
    else {
        /* allocation error */
        _sfetch_pool_discard(pool);
    }
    return pool->valid;
}

_SOKOL_PRIVATE uint32_t _sfetch_pool_item_alloc(_sfetch_pool_t* pool, const sfetch_request_t* request) {
    SOKOL_ASSERT(pool && pool->valid);
    if (pool->free_top > 0) {
        uint32_t slot_index = pool->free_slots[--pool->free_top];
        SOKOL_ASSERT((slot_index > 0) && (slot_index < pool->size));
        uint32_t slot_id = _sfetch_make_id(slot_index, ++pool->gen_ctrs[slot_index]);
        _sfetch_item_init(&pool->items[slot_index], slot_id, request);
        pool->items[slot_index].state = _SFETCH_STATE_ALLOCATED;
        return slot_id;
    }
    else {
        /* pool exhausted, return the 'invalid handle' */
        return _sfetch_make_id(0, 0);
    }
}

_SOKOL_PRIVATE void _sfetch_pool_item_free(_sfetch_pool_t* pool, uint32_t slot_id) {
    SOKOL_ASSERT(pool && pool->valid);
    uint32_t slot_index = _sfetch_slot_index(slot_id);
    SOKOL_ASSERT((slot_index > 0) && (slot_index < pool->size));
    SOKOL_ASSERT(pool->items[slot_index].handle.id == slot_id);
    #if defined(SOKOL_DEBUG)
    /* debug check against double-free */
    for (uint32_t i = 0; i < pool->free_top; i++) {
        SOKOL_ASSERT(pool->free_slots[i] != slot_index);
    }
    #endif
    _sfetch_item_discard(&pool->items[slot_index]);
    pool->free_slots[pool->free_top++] = slot_index;
    SOKOL_ASSERT(pool->free_top <= (pool->size - 1));
}

/* return pointer to item by handle without matching id check */
_SOKOL_PRIVATE _sfetch_item_t* _sfetch_pool_item_at(_sfetch_pool_t* pool, uint32_t slot_id) {
    SOKOL_ASSERT(pool && pool->valid);
    uint32_t slot_index = _sfetch_slot_index(slot_id);
    SOKOL_ASSERT((slot_index > 0) && (slot_index < pool->size));
    return &pool->items[slot_index];
}

/* return pointer to item by handle with matching id check */
_SOKOL_PRIVATE _sfetch_item_t* _sfetch_pool_item_lookup(_sfetch_pool_t* pool, uint32_t slot_id) {
    SOKOL_ASSERT(pool && pool->valid);
    if (0 != slot_id) {
        _sfetch_item_t* item = _sfetch_pool_item_at(pool, slot_id);
        if (item->handle.id == slot_id) {
            return item;
        }
    }
    return 0;
}

/*=== PLATFORM WRAPPER FUNCTIONS =============================================*/
#if _SFETCH_PLATFORM_POSIX
_SOKOL_PRIVATE _sfetch_file_handle_t _sfetch_file_open(const _sfetch_path_t* path) {
    return fopen(path->buf, "rb");
}

_SOKOL_PRIVATE void _sfetch_file_close(_sfetch_file_handle_t h) {
    fclose(h);
}

_SOKOL_PRIVATE bool _sfetch_file_handle_valid(_sfetch_file_handle_t h) {
    return h != _SFETCH_INVALID_FILE_HANDLE;
}

_SOKOL_PRIVATE uint64_t _sfetch_file_size(_sfetch_file_handle_t h) {
    fseek(h, 0, SEEK_END);
    return ftell(h);
}

_SOKOL_PRIVATE bool _sfetch_file_read(_sfetch_file_handle_t h, uint64_t offset, uint64_t num_bytes, void* ptr) {
    fseek(h, offset, SEEK_SET);
    return num_bytes == fread(ptr, 1, num_bytes, h);
}

_SOKOL_PRIVATE bool _sfetch_thread_init(_sfetch_thread_t* thread, _sfetch_thread_func_t thread_func, void* thread_arg) {
    SOKOL_ASSERT(thread && !thread->valid && !thread->stop_requested);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&thread->incoming_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&thread->outgoing_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&thread->running_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&thread->stop_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_cond_init(&thread->incoming_cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    /* FIXME: in debug mode, the threads should be named */
    pthread_mutex_lock(&thread->running_mutex);
    int res = pthread_create(&thread->thread, 0, thread_func, thread_arg);
    thread->valid = (0 == res);
    pthread_mutex_unlock(&thread->running_mutex);
    return thread->valid;
}

_SOKOL_PRIVATE void _sfetch_thread_request_stop(_sfetch_thread_t* thread) {
    pthread_mutex_lock(&thread->stop_mutex);
    thread->stop_requested = true;
    pthread_mutex_unlock(&thread->stop_mutex);
}

_SOKOL_PRIVATE bool _sfetch_thread_stop_requested(_sfetch_thread_t* thread) {
    pthread_mutex_lock(&thread->stop_mutex);
    bool stop_requested = thread->stop_requested;
    pthread_mutex_unlock(&thread->stop_mutex);
    return stop_requested;
}

_SOKOL_PRIVATE void _sfetch_thread_join(_sfetch_thread_t* thread) {
    SOKOL_ASSERT(thread);
    if (thread->valid) {
        pthread_mutex_lock(&thread->incoming_mutex);
        _sfetch_thread_request_stop(thread);
        pthread_cond_signal(&thread->incoming_cond);
        pthread_mutex_unlock(&thread->incoming_mutex);
        pthread_join(thread->thread, 0);
        thread->valid = false;
    }
    pthread_mutex_destroy(&thread->stop_mutex);
    pthread_mutex_destroy(&thread->running_mutex);
    pthread_mutex_destroy(&thread->incoming_mutex);
    pthread_mutex_destroy(&thread->outgoing_mutex);
    pthread_cond_destroy(&thread->incoming_cond);
}

/* called when the thread-func is entered, this blocks the thread func until
   the _sfetch_thread_t object is fully initialized
*/
_SOKOL_PRIVATE void _sfetch_thread_entered(_sfetch_thread_t* thread) {
    pthread_mutex_lock(&thread->running_mutex);
}

/* called by the thread-func right before it is left */
_SOKOL_PRIVATE void _sfetch_thread_leaving(_sfetch_thread_t* thread) {
    pthread_mutex_unlock(&thread->running_mutex);
}

_SOKOL_PRIVATE void _sfetch_thread_enqueue_incoming(_sfetch_thread_t* thread, _sfetch_ring_t* incoming, _sfetch_ring_t* src) {
    /* called from user thread */
    SOKOL_ASSERT(thread && thread->valid);
    SOKOL_ASSERT(incoming && incoming->buf);
    SOKOL_ASSERT(src && src->buf);
    if (!_sfetch_ring_empty(src)) {
        pthread_mutex_lock(&thread->incoming_mutex);
        while (!_sfetch_ring_full(incoming) && !_sfetch_ring_empty(src)) {
            _sfetch_ring_enqueue(incoming, _sfetch_ring_dequeue(src));
        }
        pthread_cond_signal(&thread->incoming_cond);
        pthread_mutex_unlock(&thread->incoming_mutex);
    }
}

_SOKOL_PRIVATE uint32_t _sfetch_thread_dequeue_incoming(_sfetch_thread_t* thread, _sfetch_ring_t* incoming) {
    /* called from thread function */
    SOKOL_ASSERT(thread && thread->valid);
    SOKOL_ASSERT(incoming && incoming->buf);
    pthread_mutex_lock(&thread->incoming_mutex);
    while (_sfetch_ring_empty(incoming) && !thread->stop_requested) {
        pthread_cond_wait(&thread->incoming_cond, &thread->incoming_mutex);
    }
    uint32_t item = 0;
    if (!thread->stop_requested) {
        item = _sfetch_ring_dequeue(incoming);
    }
    pthread_mutex_unlock(&thread->incoming_mutex);
    return item;
}

_SOKOL_PRIVATE bool _sfetch_thread_enqueue_outgoing(_sfetch_thread_t* thread, _sfetch_ring_t* outgoing, uint32_t item) {
    /* called from thread function */
    SOKOL_ASSERT(thread && thread->valid);
    SOKOL_ASSERT(outgoing && outgoing->buf);
    SOKOL_ASSERT(0 != item);
    pthread_mutex_lock(&thread->outgoing_mutex);
    bool result = false;
    if (!_sfetch_ring_full(outgoing)) {
        _sfetch_ring_enqueue(outgoing, item);
    }
    pthread_mutex_unlock(&thread->outgoing_mutex);
    return result;
}

_SOKOL_PRIVATE void _sfetch_thread_dequeue_outgoing(_sfetch_thread_t* thread, _sfetch_ring_t* outgoing, _sfetch_ring_t* dst) {
    /* called from user thread */
    SOKOL_ASSERT(thread && thread->valid);
    SOKOL_ASSERT(outgoing && outgoing->buf);
    SOKOL_ASSERT(dst && dst->buf);
    pthread_mutex_lock(&thread->outgoing_mutex);
    while (!_sfetch_ring_full(dst) && !_sfetch_ring_empty(outgoing)) {
        _sfetch_ring_enqueue(dst, _sfetch_ring_dequeue(outgoing));
    }
    pthread_mutex_unlock(&thread->outgoing_mutex);
}
#endif /* _SFETCH_PLATFORM_POSIX */

#if _SFETCH_PLATFORM_WINDOWS
_SOKOL_PRIVATE bool _sfetch_win32_utf8_to_wide(const char* src, wchar_t* dst, int dst_num_bytes) {
    SOKOL_ASSERT(src && dst && (dst_num_bytes > 1));
    memset(dst, 0, dst_num_bytes);
    const int dst_chars = dst_num_bytes / sizeof(wchar_t);
    const int dst_needed = MultiByteToWideChar(CP_UTF8, 0, src, -1, 0, 0);
    if ((dst_needed > 0) && (dst_needed < dst_chars)) {
        MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_chars);
        return true;
    }
    else {
        /* input string doesn't fit into destination buffer */
        return false;
    }
}

_SOKOL_PRIVATE _sfetch_file_handle_t _sfetch_file_open(const _sfetch_path_t* path) {
    wchar_t w_path[SFETCH_MAX_PATH];
    if (!_sfetch_win32_utf8_to_wide(path->buf, w_path, sizeof(w_path))) {
        SOKOL_LOG("_sfetch_file_open: error converting UTF-8 path to wide string");
        return 0;
    }
    _sfetch_file_handle_t h = CreateFileW(
        w_path,                 /* lpFileName */
        GENERIC_READ,           /* dwDesiredAccess */
        FILE_SHARE_READ,        /* dwShareMode */
        NULL,                   /* lpSecurityAttributes */
        OPEN_EXISTING,          /* dwCreationDisposition */
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,    /* dwFlagsAndAttributes */
        NULL);                  /* hTemplateFile */
    return h;
}

_SOKOL_PRIVATE void _sfetch_file_close(_sfetch_file_handle_t h) {
    CloseHandle(h);
}

_SOKOL_PRIVATE bool _sfetch_file_handle_valid(_sfetch_file_handle_t h) {
    return h != _SFETCH_INVALID_FILE_HANDLE;
}

_SOKOL_PRIVATE uint64_t _sfetch_file_size(_sfetch_file_handle_t h) {
    LARGE_INTEGER file_size;
    file_size.QuadPart = 0;
    GetFileSizeEx(h, &file_size);
    return file_size.QuadPart;
}

_SOKOL_PRIVATE bool _sfetch_file_read(_sfetch_file_handle_t h, uint64_t offset, uint64_t num_bytes, void* ptr) {
    LARGE_INTEGER offset_li;
    offset_li.QuadPart = offset;
    BOOL seek_res = SetFilePointerEx(h, offset_li, NULL, FILE_BEGIN);
    if (seek_res) {
        DWORD bytes_read = 0;
        BOOL read_res = ReadFile(h, ptr, (DWORD)num_bytes, &bytes_read, NULL);
        return read_res && (bytes_read == num_bytes);
    }
    else {
        return false;
    }
}

_SOKOL_PRIVATE bool _sfetch_thread_init(_sfetch_thread_t* thread, _sfetch_thread_func_t thread_func, void* thread_arg) {
    SOKOL_ASSERT(thread && !thread->valid && !thread->stop_requested);

    thread->incoming_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    SOKOL_ASSERT(NULL != thread->incoming_event);
    InitializeCriticalSection(&thread->incoming_critsec);
    InitializeCriticalSection(&thread->outgoing_critsec);
    InitializeCriticalSection(&thread->running_critsec);
    InitializeCriticalSection(&thread->stop_critsec);

    EnterCriticalSection(&thread->running_critsec);
    const SIZE_T stack_size = 512 * 1024;
    thread->thread = CreateThread(NULL, 512*1024, thread_func, thread_arg, 0, NULL);
    thread->valid = (NULL != thread->thread);
    LeaveCriticalSection(&thread->running_critsec);
    return thread->valid;
}

_SOKOL_PRIVATE void _sfetch_thread_request_stop(_sfetch_thread_t* thread) {
    EnterCriticalSection(&thread->stop_critsec);
    thread->stop_requested = true;
    LeaveCriticalSection(&thread->stop_critsec);
}

_SOKOL_PRIVATE bool _sfetch_thread_stop_requested(_sfetch_thread_t* thread) {
    EnterCriticalSection(&thread->stop_critsec);
    bool stop_requested = thread->stop_requested;
    LeaveCriticalSection(&thread->stop_critsec);
    return stop_requested;
}

_SOKOL_PRIVATE void _sfetch_thread_join(_sfetch_thread_t* thread) {
    if (thread->valid) {
        EnterCriticalSection(&thread->incoming_critsec);
        _sfetch_thread_request_stop(thread);
        BOOL set_event_res = SetEvent(thread->incoming_event);
        SOKOL_ASSERT(set_event_res);
        LeaveCriticalSection(&thread->incoming_critsec);
        WaitForSingleObject(thread->thread, INFINITE);
        CloseHandle(thread->thread);
        thread->valid = false;
    }
    CloseHandle(thread->incoming_event);
    DeleteCriticalSection(&thread->stop_critsec);
    DeleteCriticalSection(&thread->running_critsec);
    DeleteCriticalSection(&thread->outgoing_critsec);
    DeleteCriticalSection(&thread->incoming_critsec);
}

_SOKOL_PRIVATE void _sfetch_thread_entered(_sfetch_thread_t* thread) {
    EnterCriticalSection(&thread->running_critsec);
}

/* called by the thread-func right before it is left */
_SOKOL_PRIVATE void _sfetch_thread_leaving(_sfetch_thread_t* thread) {
    LeaveCriticalSection(&thread->running_critsec);
}

_SOKOL_PRIVATE void _sfetch_thread_enqueue_incoming(_sfetch_thread_t* thread, _sfetch_ring_t* incoming, _sfetch_ring_t* src) {
    /* called from user thread */
    SOKOL_ASSERT(thread && thread->valid);
    SOKOL_ASSERT(incoming && incoming->buf);
    SOKOL_ASSERT(src && src->buf);
    if (!_sfetch_ring_empty(src)) {
        EnterCriticalSection(&thread->incoming_critsec);
        while (!_sfetch_ring_full(incoming) && !_sfetch_ring_empty(src)) {
            _sfetch_ring_enqueue(incoming, _sfetch_ring_dequeue(src));
        }
        LeaveCriticalSection(&thread->incoming_critsec);
        BOOL set_event_res = SetEvent(thread->incoming_event);
        SOKOL_ASSERT(set_event_res);
    }
}

_SOKOL_PRIVATE uint32_t _sfetch_thread_dequeue_incoming(_sfetch_thread_t* thread, _sfetch_ring_t* incoming) {
    /* called from thread function */
    SOKOL_ASSERT(thread && thread->valid);
    SOKOL_ASSERT(incoming && incoming->buf);
    EnterCriticalSection(&thread->incoming_critsec);
    while (_sfetch_ring_empty(incoming) && !thread->stop_requested) {
        LeaveCriticalSection(&thread->incoming_critsec);
        WaitForSingleObject(&thread->incoming_event, INFINITE);
        EnterCriticalSection(&thread->incoming_critsec);
    }
    uint32_t item = 0;
    if (!thread->stop_requested) {
        item = _sfetch_ring_dequeue(incoming);
    }
    LeaveCriticalSection(&thread->incoming_critsec);
    return item;
}

_SOKOL_PRIVATE bool _sfetch_thread_enqueue_outgoing(_sfetch_thread_t* thread, _sfetch_ring_t* outgoing, uint32_t item) {
    /* called from thread function */
    SOKOL_ASSERT(thread && thread->valid);
    SOKOL_ASSERT(outgoing && outgoing->buf);
    EnterCriticalSection(&thread->outgoing_critsec);
    bool result = false;
    if (!_sfetch_ring_full(outgoing)) {
        _sfetch_ring_enqueue(outgoing, item);
    }
    LeaveCriticalSection(&thread->outgoing_critsec);
    return result;
}

_SOKOL_PRIVATE void _sfetch_thread_dequeue_outgoing(_sfetch_thread_t* thread, _sfetch_ring_t* outgoing, _sfetch_ring_t* dst) {
    /* called from user thread */
    SOKOL_ASSERT(thread && thread->valid);
    SOKOL_ASSERT(outgoing && outgoing->buf);
    SOKOL_ASSERT(dst && dst->buf);
    EnterCriticalSection(&thread->outgoing_critsec);
    while (!_sfetch_ring_full(dst) && !_sfetch_ring_empty(outgoing)) {
        _sfetch_ring_enqueue(dst, _sfetch_ring_dequeue(outgoing));
    }
    LeaveCriticalSection(&thread->outgoing_critsec);
}
#endif /* _SFETCH_PLATFORM_WINDOWS */

/*=== IO CHANNEL implementation ==============================================*/

/* per-channel request handler for native platforms accessing the local filesystem */
#if _SFETCH_HAS_THREADS
_SOKOL_PRIVATE void _sfetch_request_handler(_sfetch_t* ctx, uint32_t slot_id) {
    _sfetch_state_t state;
    _sfetch_path_t* path;
    _sfetch_item_thread_t* thread;
    _sfetch_buffer_t* buffer;
    {
        _sfetch_item_t* item = _sfetch_pool_item_lookup(&ctx->pool, slot_id);
        if (!item) {
            return;
        }
        state = item->state;
        SOKOL_ASSERT((state == _SFETCH_STATE_OPENING) ||
                     (state == _SFETCH_STATE_FETCHING) ||
                     (state == _SFETCH_STATE_PAUSED) ||
                     (state == _SFETCH_STATE_FAILED));
        path = &item->path;
        thread = &item->thread;
        buffer = &item->buffer;
    }
    if (thread->failed) {
        return;
    }
    if (state == _SFETCH_STATE_OPENING) {
        SOKOL_ASSERT(!_sfetch_file_handle_valid(thread->file_handle));
        SOKOL_ASSERT(path->buf[0]);
        SOKOL_ASSERT(thread->content_size == 0);
        SOKOL_ASSERT(thread->content_offset == 0);
        SOKOL_ASSERT(thread->fetched_size == 0);
        thread->file_handle = _sfetch_file_open(path);
        if (_sfetch_file_handle_valid(thread->file_handle)) {
            thread->content_size = _sfetch_file_size(thread->file_handle);
            /* if we already have a buffer associated with the request, skip
                the OPENED state (which only exists so the user code can look at the
                file size and provide a matching buffer), and instead start fetching
                data data immediately
            */
            if (buffer->ptr) {
                state = _SFETCH_STATE_FETCHING;
            }
        }
        else {
            thread->failed = true;
            thread->finished = true;
        }
    }
    /* may fall through from OPENING if a buffer was provided upfront */
    if (state == _SFETCH_STATE_FETCHING) {
        SOKOL_ASSERT(_sfetch_file_handle_valid(thread->file_handle));
        SOKOL_ASSERT(thread->content_size > thread->content_offset);
        if ((buffer->ptr == 0) || (buffer->size == 0)) {
            thread->failed = true;
        }
        else {
            uint64_t bytes_to_read = thread->content_size - thread->content_offset;
            if (bytes_to_read > buffer->size) {
                bytes_to_read = buffer->size;
            }
            const uint64_t offset = thread->content_offset;
            if (_sfetch_file_read(thread->file_handle, offset, bytes_to_read, buffer->ptr)) {
                thread->fetched_size = bytes_to_read;
                thread->content_offset += bytes_to_read;
            }
            else {
                thread->failed = true;
            }
        }
        if (thread->failed || (thread->content_offset >= thread->content_size)) {
            _sfetch_file_close(thread->file_handle);
            thread->file_handle = 0;
            thread->finished = true;
        }
    }
    /* ignore items in PAUSED or FAILED state */
}

#if _SFETCH_PLATFORM_WINDOWS
_SOKOL_PRIVATE DWORD WINAPI _sfetch_channel_thread_func(LPVOID arg) {
#else
_SOKOL_PRIVATE void* _sfetch_channel_thread_func(void* arg) {
#endif
    _sfetch_channel_t* chn = (_sfetch_channel_t*) arg;
    _sfetch_thread_entered(&chn->thread);
    while (!_sfetch_thread_stop_requested(&chn->thread)) {
        /* block until work arrives */
        uint32_t slot_id = _sfetch_thread_dequeue_incoming(&chn->thread, &chn->thread_incoming);
        /* slot_id will be invalid if the thread was woken up to join */
        if (!_sfetch_thread_stop_requested(&chn->thread)) {
            SOKOL_ASSERT(0 != slot_id);
            chn->request_handler(chn->ctx, slot_id);
            SOKOL_ASSERT(!_sfetch_ring_full(&chn->thread_outgoing));
            _sfetch_thread_enqueue_outgoing(&chn->thread, &chn->thread_outgoing, slot_id);
        }
    }
    _sfetch_thread_leaving(&chn->thread);
    return 0;
}
#endif /* _SFETCH_HAS_THREADS */

#if _SFETCH_PLATFORM_EMSCRIPTEN
/*=== embedded Javascript helper functions ===================================*/
EM_JS(void, sfetch_js_send_head_request, (uint32_t slot_id, const char* path_cstr), {
    var path_str = UTF8ToString(path_cstr);
    var req = new XMLHttpRequest();
    req.open('HEAD', path_str);
    req.onreadystatechange = function() {
        if (this.readyState == this.DONE) {
            if (this.status == 200) {
                var content_length = this.getResponseHeader('Content-Length');
                __sfetch_emsc_head_response(slot_id, content_length);
            }
            else {
                __sfetch_emsc_failed(slot_id);
            }
        }
    };
    req.send();
});

EM_JS(void, sfetch_js_send_range_request, (uint32_t slot_id, const char* path_cstr, int offset, int num_bytes, int content_length, void* buf_ptr), {
    var path_str = UTF8ToString(path_cstr);
    var req = new XMLHttpRequest();
    req.open('GET', path_str);
    req.responseType = 'arraybuffer';
    var need_range_request = !((offset == 0) && (num_bytes == content_length));
    if (need_range_request) {
        req.setRequestHeader('Range', 'bytes='+offset+'-'+(offset+num_bytes));
    }
    req.onreadystatechange = function() {
        if (this.readyState == this.DONE) {
            if ((this.status == 206) || ((this.status == 200) && !need_range_request)) {
                var u8_array = new Uint8Array(req.response);
                HEAPU8.set(u8_array, buf_ptr);
                __sfetch_emsc_range_response(slot_id, num_bytes);
            }
            else {
                __sfetch_emsc_failed(slot_id);
            }
        }
    };
    req.send();
});

/*=== emscripten specific C helper functions =================================*/
#ifdef __cplusplus
extern "C" {
#endif
void _sfetch_emsc_send_range_request(uint32_t slot_id, _sfetch_item_t* item) {
    SOKOL_ASSERT(item->thread.content_size > item->thread.content_offset);
    if ((item->buffer.ptr == 0) || (item->buffer.size == 0)) {
        item->thread.failed = true;
    }
    else {
        /* send a regular HTTP range request to fetch the next chunk of data
            FIXME: need to figure out a way to use 64-bit sizes here
        */
        uint32_t bytes_to_read = item->thread.content_size - item->thread.content_offset;
        if (bytes_to_read > item->buffer.size) {
            bytes_to_read = item->buffer.size;
        }
        const uint32_t offset = item->thread.content_offset;
        sfetch_js_send_range_request(slot_id, item->path.buf, offset, bytes_to_read, item->thread.content_size, item->buffer.ptr);
    }
}

/* called by JS when the initial HEAD request finished successfully */
EMSCRIPTEN_KEEPALIVE void _sfetch_emsc_head_response(uint32_t slot_id, uint32_t content_length) {
    //printf("sfetch_emsc_head_response(slot_id=%d content_length=%d)\n", slot_id, content_length);
    _sfetch_t* ctx = _sfetch_ctx();
    if (ctx && ctx->valid) {
        _sfetch_item_t* item = _sfetch_pool_item_lookup(&ctx->pool, slot_id);
        if (item) {
            item->thread.content_size = content_length;
            if (item->buffer.ptr) {
                /* if a buffer was provided, continue immediate with fetching the first
                    chunk, instead of passing the request back to the channel
                */
                _sfetch_emsc_send_range_request(slot_id, item);
            }
            else {
                /* if no buffer was provided upfront pass back to the channel so
                    that the response-user-callback can be called
                */
                _sfetch_ring_enqueue(&ctx->chn[item->channel].user_outgoing, slot_id);
            }
        }
    }
}

/* called by JS when a followup GET request finished successfully */
EMSCRIPTEN_KEEPALIVE void _sfetch_emsc_range_response(uint32_t slot_id, uint32_t num_bytes_read) {
    //printf("sfetch_emsc_range_response(slot_id=%d, num_bytes_read=%d)\n", slot_id, num_bytes_read);
    _sfetch_t* ctx = _sfetch_ctx();
    if (ctx && ctx->valid) {
        _sfetch_item_t* item = _sfetch_pool_item_lookup(&ctx->pool, slot_id);
        if (item) {
            item->thread.fetched_size = num_bytes_read;
            item->thread.content_offset += num_bytes_read;
            if (item->thread.content_offset >= item->thread.content_size) {
                item->thread.finished = true;
            }
            _sfetch_ring_enqueue(&ctx->chn[item->channel].user_outgoing, slot_id);
        }
    }
}

/* called by JS when an error occurred */
EMSCRIPTEN_KEEPALIVE void _sfetch_emsc_failed(uint32_t slot_id) {
    //printf("sfetch_emsc_failed(slot_id=%d)\n", slot_id);
    _sfetch_t* ctx = _sfetch_ctx();
    if (ctx && ctx->valid) {
        _sfetch_item_t* item = _sfetch_pool_item_lookup(&ctx->pool, slot_id);
        if (item) {
            item->thread.failed = true;
            item->thread.finished = true;
            _sfetch_ring_enqueue(&ctx->chn[item->channel].user_outgoing, slot_id);
        }
    }
}
#ifdef __cplusplus
} /* extern "C" */
#endif

_SOKOL_PRIVATE void _sfetch_request_handler(_sfetch_t* ctx, uint32_t slot_id) {
    _sfetch_item_t* item = _sfetch_pool_item_lookup(&ctx->pool, slot_id);
    if (!item) {
        return;
    }
    if (item->state == _SFETCH_STATE_OPENING) {
        SOKOL_ASSERT(item->path.buf[0]);
        /* We need to query the content-size first with a separate HEAD request,
            no matter if a buffer was provided or not (because sending a too big
            range request speculatively doesn't work). With the response, we can
            also check whether the server actually supports range requests
        */
        sfetch_js_send_head_request(slot_id, item->path.buf);
        /* see _sfetch_emsc_head_response() for the rest... */
    }
    else if (item->state == _SFETCH_STATE_FETCHING) {
        _sfetch_emsc_send_range_request(slot_id, item);
    }
    else {
        /* just move all other items (e.g. paused or cancelled)
           into the outgoing queue, so they wont get lost
        */
        _sfetch_ring_enqueue(&ctx->chn[item->channel].user_outgoing, slot_id);
    }
    if (item->thread.failed) {
        item->thread.finished = true;
    }
}
#endif

_SOKOL_PRIVATE void _sfetch_channel_discard(_sfetch_channel_t* chn) {
    SOKOL_ASSERT(chn);
    #if _SFETCH_HAS_THREADS
        if (chn->valid) {
            _sfetch_thread_join(&chn->thread);
        }
        _sfetch_ring_discard(&chn->thread_incoming);
        _sfetch_ring_discard(&chn->thread_outgoing);
    #endif
    _sfetch_ring_discard(&chn->free_lanes);
    _sfetch_ring_discard(&chn->user_sent);
    _sfetch_ring_discard(&chn->user_incoming);
    _sfetch_ring_discard(&chn->user_outgoing);
    _sfetch_ring_discard(&chn->free_lanes);
    chn->valid = false;
}

_SOKOL_PRIVATE bool _sfetch_channel_init(_sfetch_channel_t* chn, _sfetch_t* ctx, uint32_t num_items, uint32_t num_lanes, void (*request_handler)(_sfetch_t* ctx, uint32_t)) {
    SOKOL_ASSERT(chn && (num_items > 0) && request_handler);
    SOKOL_ASSERT(!chn->valid);
    bool valid = true;
    chn->request_handler = request_handler;
    chn->ctx = ctx;
    valid &= _sfetch_ring_init(&chn->free_lanes, num_lanes);
    for (uint32_t lane = 0; lane < num_lanes; lane++) {
        _sfetch_ring_enqueue(&chn->free_lanes, lane);
    }
    valid &= _sfetch_ring_init(&chn->user_sent, num_items);
    valid &= _sfetch_ring_init(&chn->user_incoming, num_lanes);
    valid &= _sfetch_ring_init(&chn->user_outgoing, num_lanes);
    #if _SFETCH_HAS_THREADS
        valid &= _sfetch_ring_init(&chn->thread_incoming, num_lanes);
        valid &= _sfetch_ring_init(&chn->thread_outgoing, num_lanes);
    #endif
    if (valid) {
        chn->valid = true;
        #if _SFETCH_HAS_THREADS
        _sfetch_thread_init(&chn->thread, _sfetch_channel_thread_func, chn);
        #endif
        return true;
    }
    else {
        _sfetch_channel_discard(chn);
        return false;
    }
}

/* put a request into the channels sent-queue, this is where all new requests
   are stored until a lane becomes free.
*/
_SOKOL_PRIVATE bool _sfetch_channel_send(_sfetch_channel_t* chn, uint32_t slot_id) {
    SOKOL_ASSERT(chn && chn->valid);
    if (!_sfetch_ring_full(&chn->user_sent)) {
        _sfetch_ring_enqueue(&chn->user_sent, slot_id);
        return true;
    }
    else {
        SOKOL_LOG("sfetch_send: user_sent queue is full)");
        return false;
    }
}

/* per-frame channel stuff: move requests in and out of the IO threads, call response callbacks */
_SOKOL_PRIVATE void _sfetch_channel_dowork(_sfetch_channel_t* chn, _sfetch_pool_t* pool) {

    /* move items from sent- to incoming-queue permitting free lanes */
    const uint32_t num_sent = _sfetch_ring_count(&chn->user_sent);
    const uint32_t avail_lanes = _sfetch_ring_count(&chn->free_lanes);
    const uint32_t num_move = (num_sent < avail_lanes) ? num_sent : avail_lanes;
    for (uint32_t i = 0; i < num_move; i++) {
        const uint32_t slot_id = _sfetch_ring_dequeue(&chn->user_sent);
        _sfetch_item_t* item = _sfetch_pool_item_lookup(pool, slot_id);
        SOKOL_ASSERT(item);
        item->lane = _sfetch_ring_dequeue(&chn->free_lanes);
        _sfetch_ring_enqueue(&chn->user_incoming, slot_id);
    }

    /* prepare incoming items for being moved into the IO thread */
    const uint32_t num_incoming = _sfetch_ring_count(&chn->user_incoming);
    for (uint32_t i = 0; i < num_incoming; i++) {
        const uint32_t slot_id = _sfetch_ring_peek(&chn->user_incoming, i);
        _sfetch_item_t* item = _sfetch_pool_item_lookup(pool, slot_id);
        SOKOL_ASSERT(item);
        SOKOL_ASSERT(item->state != _SFETCH_STATE_INITIAL);
        SOKOL_ASSERT(item->state != _SFETCH_STATE_OPENING);
        SOKOL_ASSERT(item->state != _SFETCH_STATE_FETCHING);
        /* transfer input params from user- to thread-data */
        if (item->user.pause) {
            item->state = _SFETCH_STATE_PAUSED;
            item->user.pause = false;
        }
        if (item->user.cont) {
            if (item->state == _SFETCH_STATE_PAUSED) {
                item->state = _SFETCH_STATE_FETCHED;
            }
            item->user.cont = false;
        }
        if (item->user.cancel) {
            item->state = _SFETCH_STATE_FAILED;
            item->user.finished = true;
        }
        switch (item->state) {
            case _SFETCH_STATE_ALLOCATED:
                item->state = _SFETCH_STATE_OPENING;
                break;
            case _SFETCH_STATE_OPENED:
            case _SFETCH_STATE_FETCHED:
                item->state = _SFETCH_STATE_FETCHING;
                break;
            default: break;
        }
    }

    #if _SFETCH_HAS_THREADS
        /* move new items into the IO threads and processed items out of IO threads */
        _sfetch_thread_enqueue_incoming(&chn->thread, &chn->thread_incoming, &chn->user_incoming);
        _sfetch_thread_dequeue_outgoing(&chn->thread, &chn->thread_outgoing, &chn->user_outgoing);
    #else
        /* without threading just directly dequeue items from the user_incoming queue and
           call the request handler, the user_outgoing queue will be filled as the
           asynchronous HTTP requests sent by the request handler are completed
        */
        while (!_sfetch_ring_empty(&chn->user_incoming)) {
            uint32_t slot_id = _sfetch_ring_dequeue(&chn->user_incoming);
            _sfetch_request_handler(chn->ctx, slot_id);
        }
    #endif

    /* drain the outgoing queue, prepare items for invoking the response
       callback, and finally call the response callback, free finished items
    */
    while (!_sfetch_ring_empty(&chn->user_outgoing)) {
        const uint32_t slot_id = _sfetch_ring_dequeue(&chn->user_outgoing);
        SOKOL_ASSERT(slot_id);
        _sfetch_item_t* item = _sfetch_pool_item_lookup(pool, slot_id);
        SOKOL_ASSERT(item && item->callback);
        SOKOL_ASSERT(item->state != _SFETCH_STATE_INITIAL);
        SOKOL_ASSERT(item->state != _SFETCH_STATE_ALLOCATED);
        SOKOL_ASSERT(item->state != _SFETCH_STATE_OPENED);
        SOKOL_ASSERT(item->state != _SFETCH_STATE_FETCHED);
        /* transfer output params from thread- to user-data */
        item->user.content_size = item->thread.content_size;
        item->user.content_offset = item->thread.content_offset;
        item->user.fetched_size  = item->thread.fetched_size;
        if (item->thread.finished) {
            item->user.finished = true;
        }
        /* state transition */
        if (item->thread.failed) {
            item->state = _SFETCH_STATE_FAILED;
        }
        else {
            switch (item->state) {
                case _SFETCH_STATE_OPENING:
                    /* if the request already had a buffer provided, the
                       OPENING state already has fetched data and we shortcut
                       to the first FETCHED state to shorten the time a request occupies
                       a lane, otherwise, invoke the callback with OPENED state
                       so it can provide a buffer
                    */
                    if (item->user.content_offset > 0) {
                        item->state = _SFETCH_STATE_FETCHED;
                    }
                    else {
                        item->state = _SFETCH_STATE_OPENED;
                    }
                    break;
                case _SFETCH_STATE_FETCHING:
                    item->state = _SFETCH_STATE_FETCHED;
                    break;
                default:
                    break;
            }
        }
        /* invoke response callback */
        sfetch_response_t response;
        memset(&response, 0, sizeof(response));
        response.handle.id = slot_id;
        response.opened = (item->state == _SFETCH_STATE_OPENED);
        response.fetched = (item->state == _SFETCH_STATE_FETCHED);
        response.paused = (item->state == _SFETCH_STATE_PAUSED);
        response.finished = item->user.finished;
        response.failed = (item->state == _SFETCH_STATE_FAILED);
        response.cancelled = item->user.cancel;
        response.channel = item->channel;
        response.lane = item->lane;
        response.path = item->path.buf;
        response.user_data = item->user.user_data;
        response.content_size = item->user.content_size;
        response.content_offset = item->user.content_offset - item->user.fetched_size;
        response.fetched_size = item->user.fetched_size;
        response.buffer_ptr = item->buffer.ptr;
        response.buffer_size = item->buffer.size;
        item->callback(&response);

        /* when the request is finish, free the lane for another request,
           otherwise feed it back into the incoming queue
        */
        if (item->user.finished) {
            _sfetch_ring_enqueue(&chn->free_lanes, item->lane);
            _sfetch_pool_item_free(pool, slot_id);
        }
        else {
            _sfetch_ring_enqueue(&chn->user_incoming, slot_id);
        }
    }
}

/*=== private high-level functions ===========================================*/
_SOKOL_PRIVATE bool _sfetch_validate_request(_sfetch_t* ctx, const sfetch_request_t* req) {
    #if defined(SOKOL_DEBUG)
    if (req->channel >= ctx->desc.num_channels) {
        SOKOL_LOG("_sfetch_validate_request: request.num_channels too big!");
        return false;
    }
    if (!req->path) {
        SOKOL_LOG("_sfetch_validate_request: request.path is null!");
        return false;
    }
    if (strlen(req->path) >= (SFETCH_MAX_PATH-1)) {
        SOKOL_LOG("_sfetch_validate_request: request.path is too long (must be < SFETCH_MAX_PATH-1)");
        return false;
    }
    if (!req->callback) {
        SOKOL_LOG("_sfetch_validate_request: request.callback missing");
        return false;
    }
    if (req->user_data_ptr && (req->user_data_size == 0)) {
        SOKOL_LOG("_sfetch_validate_request: request.user_data_ptr is set, but req.user_data_size is null");
        return false;
    }
    if (!req->user_data_ptr && (req->user_data_size > 0)) {
        SOKOL_LOG("_sfetch_validate_request: request.user_data_ptr is null, but req.user_data_size is not");
        return false;
    }
    if (req->user_data_size > SFETCH_MAX_USERDATA_UINT64 * sizeof(uint64_t)) {
        SOKOL_LOG("_sfetch_validate_request: request.user_data_size is too big (see SFETCH_MAX_USERDATA_UINT64");
        return false;
    }
    #endif
    return true;
}

/*=== PUBLIC API FUNCTIONS ===================================================*/
SOKOL_API_IMPL void sfetch_setup(const sfetch_desc_t* desc) {
    SOKOL_ASSERT(desc);
    SOKOL_ASSERT((desc->_start_canary == 0) && (desc->_end_canary == 0));
    SOKOL_ASSERT(0 == _sfetch);
    _sfetch = (_sfetch_t*) SOKOL_MALLOC(sizeof(_sfetch_t));
    SOKOL_ASSERT(_sfetch);
    memset(_sfetch, 0, sizeof(_sfetch_t));
    _sfetch_t* ctx = _sfetch_ctx();
    ctx->desc = *desc;
    ctx->setup = true;
    ctx->valid = true;

    /* replace zero-init items with default values */
    ctx->desc.max_requests = _sfetch_def(ctx->desc.max_requests, 128);
    ctx->desc.num_channels = _sfetch_def(ctx->desc.num_channels, 1);
    ctx->desc.num_lanes = _sfetch_def(ctx->desc.num_lanes, 1);
    if (ctx->desc.num_channels > SFETCH_MAX_CHANNELS) {
        ctx->desc.num_channels = SFETCH_MAX_CHANNELS;
        SOKOL_LOG("sfetch_setup: clamping num_channels to SFETCH_MAX_CHANNELS");
    }

    /* setup the global request item pool */
    ctx->valid &= _sfetch_pool_init(&ctx->pool, ctx->desc.max_requests);

    /* setup IO channels (one thread per channel) */
    for (uint32_t i = 0; i < ctx->desc.num_channels; i++) {
        ctx->valid &= _sfetch_channel_init(&ctx->chn[i], ctx, ctx->desc.max_requests, ctx->desc.num_lanes, _sfetch_request_handler);
    }
}

SOKOL_API_IMPL void sfetch_shutdown(void) {
    _sfetch_t* ctx = _sfetch_ctx();
    SOKOL_ASSERT(ctx && ctx->setup);
    ctx->valid = false;
    /* IO threads must be shutdown first */
    for (uint32_t i = 0; i < ctx->desc.num_channels; i++) {
        if (ctx->chn[i].valid) {
            _sfetch_channel_discard(&ctx->chn[i]);
        }
    }
    _sfetch_pool_discard(&ctx->pool);
    ctx->setup = false;
    SOKOL_FREE(ctx);
    _sfetch = 0;
}

SOKOL_API_IMPL bool sfetch_valid(void) {
    _sfetch_t* ctx = _sfetch_ctx();
    return ctx && ctx->valid;
}

SOKOL_API_IMPL sfetch_desc_t sfetch_desc(void) {
    _sfetch_t* ctx = _sfetch_ctx();
    SOKOL_ASSERT(ctx && ctx->valid);
    return ctx->desc;
}

SOKOL_API_IMPL int sfetch_max_userdata_bytes(void) {
    return SFETCH_MAX_USERDATA_UINT64 * 8;
}

SOKOL_API_IMPL int sfetch_max_path(void) {
    return SFETCH_MAX_PATH;
}

SOKOL_API_IMPL bool sfetch_handle_valid(sfetch_handle_t h) {
    _sfetch_t* ctx = _sfetch_ctx();
    SOKOL_ASSERT(ctx && ctx->valid);
    /* shortcut invalid handle */
    if (h.id == 0) {
        return false;
    }
    return 0 != _sfetch_pool_item_lookup(&ctx->pool, h.id);
}

SOKOL_API_IMPL sfetch_handle_t sfetch_send(const sfetch_request_t* request) {
    _sfetch_t* ctx = _sfetch_ctx();
    SOKOL_ASSERT(ctx && ctx->setup);
    SOKOL_ASSERT(request && (request->_start_canary == 0) && (request->_end_canary == 0));

    const sfetch_handle_t invalid_handle = _sfetch_make_handle(0);
    if (!ctx->valid) {
        return invalid_handle;
    }
    if (!_sfetch_validate_request(ctx, request)) {
        return invalid_handle;
    }
    SOKOL_ASSERT(request->channel < ctx->desc.num_channels);

    uint32_t slot_id = _sfetch_pool_item_alloc(&ctx->pool, request);
    if (0 == slot_id) {
        SOKOL_LOG("sfetch_send: request pool exhausted (too many active requests)");
        return invalid_handle;
    }
    if (!_sfetch_channel_send(&ctx->chn[request->channel], slot_id)) {
        /* send failed because the channels sent-queue overflowed */
        _sfetch_pool_item_free(&ctx->pool, slot_id);
        return invalid_handle;
    }
    return _sfetch_make_handle(slot_id);
}

SOKOL_API_IMPL void sfetch_dowork(void) {
    _sfetch_t* ctx = _sfetch_ctx();
    SOKOL_ASSERT(ctx && ctx->setup);
    if (!ctx->valid) {
        return;
    }
    /* we're pumping each channel 2x so that unfinished request items coming out the
       IO threads can be moved back into the IO-thread immediately without
       having to wait a frame
     */
    ctx->in_callback = true;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t chn_index = 0; chn_index < ctx->desc.num_channels; chn_index++) {
            _sfetch_channel_dowork(&ctx->chn[chn_index], &ctx->pool);
        }
    }
    ctx->in_callback = false;
}

SOKOL_API_IMPL void sfetch_bind_buffer(sfetch_handle_t h, void* buffer_ptr, uint64_t buffer_size) {
    _sfetch_t* ctx = _sfetch_ctx();
    SOKOL_ASSERT(ctx && ctx->valid);
    SOKOL_ASSERT(ctx->in_callback);
    _sfetch_item_t* item = _sfetch_pool_item_lookup(&ctx->pool, h.id);
    if (item) {
        SOKOL_ASSERT((0 == item->buffer.ptr) && (0 == item->buffer.size));
        item->buffer.ptr = (uint8_t*) buffer_ptr;
        item->buffer.size = buffer_size;
    }
}

SOKOL_API_IMPL void* sfetch_unbind_buffer(sfetch_handle_t h) {
    _sfetch_t* ctx = _sfetch_ctx();
    SOKOL_ASSERT(ctx && ctx->valid);
    SOKOL_ASSERT(ctx->in_callback);
    _sfetch_item_t* item = _sfetch_pool_item_lookup(&ctx->pool, h.id);
    if (item) {
        void* prev_buf_ptr = item->buffer.ptr;
        item->buffer.ptr = 0;
        item->buffer.size = 0;
        return prev_buf_ptr;
    }
    else {
        return 0;
    }
}

SOKOL_API_IMPL void sfetch_pause(sfetch_handle_t h) {
    _sfetch_t* ctx = _sfetch_ctx();
    SOKOL_ASSERT(ctx && ctx->valid);
    _sfetch_item_t* item = _sfetch_pool_item_lookup(&ctx->pool, h.id);
    if (item) {
        item->user.pause = true;
        item->user.cont = false;
    }
}

SOKOL_API_IMPL void sfetch_continue(sfetch_handle_t h) {
    _sfetch_t* ctx = _sfetch_ctx();
    SOKOL_ASSERT(ctx && ctx->valid);
    _sfetch_item_t* item = _sfetch_pool_item_lookup(&ctx->pool, h.id);
    if (item) {
        item->user.cont = true;
        item->user.pause = false;
    }
}

SOKOL_API_IMPL void sfetch_cancel(sfetch_handle_t h) {
    _sfetch_t* ctx = _sfetch_ctx();
    SOKOL_ASSERT(ctx && ctx->valid);
    _sfetch_item_t* item = _sfetch_pool_item_lookup(&ctx->pool, h.id);
    if (item) {
        item->user.cont = false;
        item->user.pause = false;
        item->user.cancel = true;
    }
}

#endif /* SOKOL_IMPL */
