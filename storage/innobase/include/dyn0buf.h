/*****************************************************************************

Copyright (c) 2013, 2016, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/dyn0buf.h
The dynamically allocated buffer implementation

Created 2013-03-16 Sunny Bains
*******************************************************/

#ifndef dyn0buf_h
#define dyn0buf_h

#include "univ.i"
#include "ut0lst.h"
#include "mem0mem.h"
#include "dyn0types.h"
#include "stdlib.h"

/** Class that manages dynamic buffers. It uses a UT_LIST of
dyn_buf_t::block_t instances. We don't use STL containers in
order to avoid the overhead of heap calls. Using a custom memory
allocator doesn't solve the problem either because we have to get
the memory from somewhere. We can't use the block_t::m_data as the
backend for the custom allocator because we would like the data in
the blocks to be contiguous. */
template <size_t SIZE = DYN_ARRAY_DATA_SIZE>
class dyn_buf_t {
public:

	class block_t;

	typedef UT_LIST_NODE_T(block_t) block_node_t;
	typedef UT_LIST_BASE_NODE_T(block_t) block_list_t;

	class block_t {
	public:

		block_t()
		{
			ut_ad(MAX_DATA_SIZE <= (2 << 15));
			init();
		}

		~block_t() { }

		/**
		Gets the number of used bytes in a block.
		@return	number of bytes used */
		ulint used() const
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(static_cast<ulint>(m_used & ~DYN_BLOCK_FULL_FLAG));
		}

		/**
		Gets pointer to the start of data.
		@return	pointer to data */
		byte* start()
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(m_data);
		}

		/**
		@return start of data - non const version */
		byte* begin()
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(m_data);
		}

		/**
		@return end of used data - non const version */
		byte* end()
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(begin() + m_used);
		}

		/**
		@return start of data - const version */
		const byte* begin() const
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(m_data);
		}

		/**
		@return end of used data - const version */
		const byte* end() const
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(begin() + m_used);
		}

	private:
		/**
		@return pointer to start of reserved space */
		template <typename Type>
		Type push(ib_uint32_t size)
		{
			Type	ptr = reinterpret_cast<Type>(end());

			m_used += size;
			ut_ad(m_used <= static_cast<ib_uint32_t>(MAX_DATA_SIZE));

			return(ptr);
		}

		/**
		Grow the stack. */
		void close(const byte* ptr)
		{
			/* Check that it is within bounds */
			ut_ad(ptr >= begin());
			ut_ad(ptr <= begin() + m_buf_end);

			/* We have done the boundary check above */
			m_used = static_cast<ib_uint32_t>(ptr - begin());

			ut_ad(m_used <= MAX_DATA_SIZE);
			ut_d(m_buf_end = 0);
		}

		/**
		Initialise the block */
		void init()
		{
			m_used = 0;
			ut_d(m_buf_end = 0);
			ut_d(m_magic_n = DYN_BLOCK_MAGIC_N);
		}
	private:
#ifdef UNIV_DEBUG
		/** If opened then this is the buffer end offset, else 0 */
		ulint		m_buf_end;

		/** Magic number (DYN_BLOCK_MAGIC_N) */
		ulint		m_magic_n;
#endif /* UNIV_DEBUG */

		/** SIZE - sizeof(m_node) + sizeof(m_used) */
		enum {
			MAX_DATA_SIZE = SIZE
				      - sizeof(block_node_t)
				      + sizeof(ib_uint32_t)
		};

		/** Storage */
		byte		m_data[MAX_DATA_SIZE];

		/** Doubly linked list node. */
		block_node_t	m_node;

		/** number of data bytes used in this block;
		DYN_BLOCK_FULL_FLAG is set when the block becomes full */
		ib_uint32_t	m_used;

		friend class dyn_buf_t;
	};

	enum { MAX_DATA_SIZE = block_t::MAX_DATA_SIZE};

	/** Default constructor */
	dyn_buf_t()
		:
		m_heap(),
		m_size()
	{
		UT_LIST_INIT(m_list, &block_t::m_node);
		push_back(&m_first_block);
	}

	/** Destructor */
	~dyn_buf_t()
	{
		erase();
	}

	/** Reset the buffer vector */
	void erase()
	{
		if (m_heap != NULL) {
			mem_heap_free(m_heap);
			m_heap = NULL;

			/* Initialise the list and add the first block. */
			UT_LIST_INIT(m_list, &block_t::m_node);
			push_back(&m_first_block);
		} else {
			m_first_block.init();
			ut_ad(UT_LIST_GET_LEN(m_list) == 1);
		}

		m_size = 0;
	}

	/**
	Makes room on top and returns a pointer to a buffer in it. After
	copying the elements, the caller must close the buffer using close().
	@param size	in bytes of the buffer; MUST be <= MAX_DATA_SIZE!
	@return	pointer to the buffer */
	byte* open(ulint size)
		MY_ATTRIBUTE((warn_unused_result))
	{
		ut_ad(size > 0);
		ut_ad(size <= MAX_DATA_SIZE);

		block_t*	block;

		block = has_space(size) ? back() : add_block();

		ut_ad(block->m_used <= MAX_DATA_SIZE);
		ut_d(block->m_buf_end = block->m_used + size);

		return(block->end());
	}

	/**
	Closes the buffer returned by open.
	@param ptr	end of used space */
	void close(const byte* ptr)
	{
		ut_ad(UT_LIST_GET_LEN(m_list) > 0);
		block_t*	block = back();

		m_size -= block->used();

		block->close(ptr);

		m_size += block->used();
	}

	/**
	Makes room on top and returns a pointer to the added element.
	The caller must copy the element to the pointer returned.
	@param size	in bytes of the element
	@return	pointer to the element */
	template <typename Type>
	Type push(ib_uint32_t size)
	{
		ut_ad(size > 0);
		ut_ad(size <= MAX_DATA_SIZE);

		block_t*	block;

		block = has_space(size) ? back() : add_block();

		m_size += size;

		/* See ISO C++03 14.2/4 for why "template" is required. */

		return(block->template push<Type>(size));
	}

	/**
	Pushes n bytes.
	@param str	string to write
	@param len	string length */
	void push(const byte* ptr, ib_uint32_t len)
	{
		while (len > 0) {
			ib_uint32_t	n_copied;

			if (len >= MAX_DATA_SIZE) {
				n_copied = MAX_DATA_SIZE;
			} else {
				n_copied = len;
			}

			::memmove(push<byte*>(n_copied), ptr, n_copied);

			ptr += n_copied;
			len -= n_copied;
		}
	}

	/**
	Returns a pointer to an element in the buffer. const version.
	@param pos	position of element in bytes from start
	@return	pointer to element */
	template <typename Type>
	const Type at(ulint pos) const
	{
		block_t*	block = const_cast<block_t*>(
			const_cast<dyn_buf_t*>(this)->find(pos));

		return(reinterpret_cast<Type>(block->begin() + pos));
	}

	/**
	Returns a pointer to an element in the buffer. non const version.
	@param pos	position of element in bytes from start
	@return	pointer to element */
	template <typename Type>
	Type at(ulint pos)
	{
		block_t*	block = const_cast<block_t*>(find(pos));

		return(reinterpret_cast<Type>(block->begin() + pos));
	}

	/**
	Returns the size of the total stored data.
	@return	data size in bytes */
	ulint size() const
		MY_ATTRIBUTE((warn_unused_result))
	{
#ifdef UNIV_DEBUG
		ulint	total_size = 0;

		for (const block_t* block = UT_LIST_GET_FIRST(m_list);
		     block != NULL;
		     block = UT_LIST_GET_NEXT(m_node, block)) {

			total_size += block->used();
		}

		ut_ad(total_size == m_size);
#endif /* UNIV_DEBUG */
		return(m_size);
	}

	/**
	Iterate over each block and call the functor.
	@return	false if iteration was terminated. */
	template <typename Functor>
	bool for_each_block(Functor& functor) const
	{
		for (const block_t* block = UT_LIST_GET_FIRST(m_list);
		     block != NULL;
		     block = UT_LIST_GET_NEXT(m_node, block)) {

			if (!functor(block)) {
				return(false);
			}
		}

		return(true);
	}

	/**
	Iterate over all the blocks in reverse and call the iterator
	@return	false if iteration was terminated. */
	template <typename Functor>
	bool for_each_block_in_reverse(Functor& functor) const
	{
		for (block_t* block = UT_LIST_GET_LAST(m_list);
		     block != NULL;
		     block = UT_LIST_GET_PREV(m_node, block)) {

			if (!functor(block)) {
				return(false);
			}
		}

		return(true);
	}

	/**
	@return the first block */
	block_t* front()
		MY_ATTRIBUTE((warn_unused_result))
	{
		ut_ad(UT_LIST_GET_LEN(m_list) > 0);
		return(UT_LIST_GET_FIRST(m_list));
	}

	/**
	@return true if m_first_block block was not filled fully */
	bool is_small() const
		MY_ATTRIBUTE((warn_unused_result))
	{
		return(m_heap == NULL);
	}

private:
	// Disable copying
	dyn_buf_t(const dyn_buf_t&);
	dyn_buf_t& operator=(const dyn_buf_t&);

	/**
	Add the block to the end of the list*/
	void push_back(block_t* block)
	{
		block->init();

		UT_LIST_ADD_LAST(m_list, block);
	}

	/** @return the last block in the list */
	block_t* back()
	{
		return(UT_LIST_GET_LAST(m_list));
	}

	/*
	@return true if request can be fullfilled */
	bool has_space(ulint size) const
	{
		return(back()->m_used + size <= MAX_DATA_SIZE);
	}

	/*
	@return true if request can be fullfilled */
	bool has_space(ulint size)
	{
		return(back()->m_used + size <= MAX_DATA_SIZE);
	}

	/** Find the block that contains the pos.
	@param pos	absolute offset, it is updated to make it relative
			to the block
	@return the block containing the pos. */
	block_t* find(ulint& pos)
	{
		block_t*	block;

		ut_ad(UT_LIST_GET_LEN(m_list) > 0);

		for (block = UT_LIST_GET_FIRST(m_list);
		     block != NULL;
		     block = UT_LIST_GET_NEXT(m_node, block)) {

			if (pos < block->used()) {
				break;
			}

			pos -= block->used();
		}

		ut_ad(block != NULL);
		ut_ad(block->used() >= pos);

		return(block);
	}

	/**
	Allocate and add a new block to m_list */
	block_t* add_block()
	{
		block_t*	block;

		if (m_heap == NULL) {
			m_heap = mem_heap_create(sizeof(*block));
		}

		block = reinterpret_cast<block_t*>(
			mem_heap_alloc(m_heap, sizeof(*block)));

		push_back(block);
		return(block);
	}

private:
	/** Heap to use for memory allocation */
	mem_heap_t*		m_heap;

	/** Allocated blocks */
	block_list_t		m_list;

	/** Total size used by all blocks */
	ulint			m_size;

	/** The default block, should always be the first element. This
	is for backwards compatibility and to avoid an extra heap allocation
	for small REDO log records */
	block_t			m_first_block;
};

typedef dyn_buf_t<DYN_ARRAY_DATA_SIZE> mtr_buf_t;

/** mtr_buf_t copier */
struct mtr_buf_copy_t {
	/** The copied buffer */
	mtr_buf_t	m_buf;
	ulint size;

	/** Append a block to the redo log buffer.
	@return whether the appending should continue (always true here) */
	bool operator()(const mtr_buf_t::block_t* block)
	{
		byte*	buf = m_buf.open(block->used());
		memcpy(buf, block->begin(), block->used());
		m_buf.close(buf + block->used());
		size += block->used();
		return(true);
	}
};

#ifdef UNIV_NVDIMM_PPL
template <size_t SIZE = PPL_DYN_ARRAY_DATA_SIZE>
class ppl_dyn_buf_t {
public:

	class block_t;

	typedef UT_LIST_NODE_T(block_t) block_node_t;
	typedef UT_LIST_BASE_NODE_T(block_t) block_list_t;

	class block_t {
	public:

		block_t()
		{
			ut_ad(MAX_DATA_SIZE <= (2 << 15));
			init();
		}

		~block_t() { }

		/**
		Gets the number of used bytes in a block.
		@return	number of bytes used */
		ulint used() const
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(static_cast<ulint>(m_used & ~DYN_BLOCK_FULL_FLAG));
		}

		/**
		Gets pointer to the start of data.
		@return	pointer to data */
		byte* start()
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(m_data);
		}

		/**
		@return start of data - non const version */
		byte* begin()
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(m_data);
		}

		/**
		@return end of used data - non const version */
		byte* end()
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(begin() + m_used);
		}

		/**
		@return start of data - const version */
		const byte* begin() const
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(m_data);
		}

		/**
		@return end of used data - const version */
		const byte* end() const
			MY_ATTRIBUTE((warn_unused_result))
		{
			return(begin() + m_used);
		}

	private:
		/**
		@return pointer to start of reserved space */
		template <typename Type>
		Type push(ib_uint32_t size)
		{
			Type	ptr = reinterpret_cast<Type>(end());

			m_used += size;
			ut_ad(m_used <= static_cast<ib_uint32_t>(MAX_DATA_SIZE));

			return(ptr);
		}

		/**
		Grow the stack. */
		void close(const byte* ptr)
		{
			/* Check that it is within bounds */
			ut_ad(ptr >= begin());
			ut_ad(ptr <= begin() + m_buf_end);

			/* We have done the boundary check above */
			m_used = static_cast<ib_uint32_t>(ptr - begin());

			ut_ad(m_used <= MAX_DATA_SIZE);
			ut_d(m_buf_end = 0);
		}

		/**
		Initialise the block */
		void init()
		{
			m_used = 0;
			ut_d(m_buf_end = 0);
			ut_d(m_magic_n = DYN_BLOCK_MAGIC_N);
		}
	private:
#ifdef UNIV_DEBUG
		/** If opened then this is the buffer end offset, else 0 */
		ulint		m_buf_end;

		/** Magic number (DYN_BLOCK_MAGIC_N) */
		ulint		m_magic_n;
#endif /* UNIV_DEBUG */

		/** SIZE - sizeof(m_node) + sizeof(m_used) */
		enum {
			MAX_DATA_SIZE = SIZE
				      - sizeof(block_node_t)
				      + sizeof(ib_uint32_t)
		};

		/** Storage */
		byte		m_data[MAX_DATA_SIZE];

		/** Doubly linked list node. */
		block_node_t	m_node;

		/** number of data bytes used in this block;
		DYN_BLOCK_FULL_FLAG is set when the block becomes full */
		ib_uint32_t	m_used;

		friend class ppl_dyn_buf_t;
	};

	enum { MAX_DATA_SIZE = block_t::MAX_DATA_SIZE};

	/** Default constructor */
	ppl_dyn_buf_t()
		:
		m_heap(),
		m_size()
	{
		UT_LIST_INIT(m_list, &block_t::m_node);
		push_back(&m_first_block);
	}

	/** Destructor */
	~ppl_dyn_buf_t()
	{
		erase();
	}

	/** Reset the buffer vector */
	void erase()
	{
		if (m_heap != NULL) {
			mem_heap_free(m_heap);
			m_heap = NULL;

			/* Initialise the list and add the first block. */
			UT_LIST_INIT(m_list, &block_t::m_node);
			push_back(&m_first_block);
		} else {
			m_first_block.init();
			ut_ad(UT_LIST_GET_LEN(m_list) == 1);
		}

		m_size = 0;
	}

	/**
	Makes room on top and returns a pointer to a buffer in it. After
	copying the elements, the caller must close the buffer using close().
	@param size	in bytes of the buffer; MUST be <= MAX_DATA_SIZE!
	@return	pointer to the buffer */
	byte* open(ulint size)
		MY_ATTRIBUTE((warn_unused_result))
	{
		ut_ad(size > 0);
		ut_ad(size <= MAX_DATA_SIZE);

		block_t*	block;

		block = has_space(size) ? back() : add_block();

		ut_ad(block->m_used <= MAX_DATA_SIZE);
		ut_d(block->m_buf_end = block->m_used + size);

		return(block->end());
	}

	/**
	Closes the buffer returned by open.
	@param ptr	end of used space */
	void close(const byte* ptr)
	{
		ut_ad(UT_LIST_GET_LEN(m_list) > 0);
		block_t*	block = back();

		m_size -= block->used();

		block->close(ptr);

		m_size += block->used();
	}

	/**
	Makes room on top and returns a pointer to the added element.
	The caller must copy the element to the pointer returned.
	@param size	in bytes of the element
	@return	pointer to the element */
	template <typename Type>
	Type push(ib_uint32_t size)
	{
		ut_ad(size > 0);
		ut_ad(size <= MAX_DATA_SIZE);

		block_t*	block;

		block = has_space(size) ? back() : add_block();

		m_size += size;

		/* See ISO C++03 14.2/4 for why "template" is required. */

		return(block->template push<Type>(size));
	}

	/**
	Pushes n bytes.
	@param str	string to write
	@param len	string length */
	void push(const byte* ptr, ib_uint32_t len)
	{
		while (len > 0) {
			ib_uint32_t	n_copied;

			if (len >= MAX_DATA_SIZE) {
				n_copied = MAX_DATA_SIZE;
			} else {
				n_copied = len;
			}

			::memmove(push<byte*>(n_copied), ptr, n_copied);

			ptr += n_copied;
			len -= n_copied;
		}
	}

	/**
	Returns a pointer to an element in the buffer. const version.
	@param pos	position of element in bytes from start
	@return	pointer to element */
	template <typename Type>
	const Type at(ulint pos) const
	{
		block_t*	block = const_cast<block_t*>(
			const_cast<ppl_dyn_buf_t*>(this)->find(pos));

		return(reinterpret_cast<Type>(block->begin() + pos));
	}

	/**
	Returns a pointer to an element in the buffer. non const version.
	@param pos	position of element in bytes from start
	@return	pointer to element */
	template <typename Type>
	Type at(ulint pos)
	{
		block_t*	block = const_cast<block_t*>(find(pos));

		return(reinterpret_cast<Type>(block->begin() + pos));
	}

	/**
	Returns the size of the total stored data.
	@return	data size in bytes */
	ulint size() const
		MY_ATTRIBUTE((warn_unused_result))
	{
#ifdef UNIV_DEBUG
		ulint	total_size = 0;

		for (const block_t* block = UT_LIST_GET_FIRST(m_list);
		     block != NULL;
		     block = UT_LIST_GET_NEXT(m_node, block)) {

			total_size += block->used();
		}

		ut_ad(total_size == m_size);
#endif /* UNIV_DEBUG */
		return(m_size);
	}

	/**
	Iterate over each block and call the functor.
	@return	false if iteration was terminated. */
	template <typename Functor>
	bool for_each_block(Functor& functor) const
	{
		for (const block_t* block = UT_LIST_GET_FIRST(m_list);
		     block != NULL;
		     block = UT_LIST_GET_NEXT(m_node, block)) {

			if (!functor(block)) {
				return(false);
			}
		}

		return(true);
	}

	/**
	Iterate over all the blocks in reverse and call the iterator
	@return	false if iteration was terminated. */
	template <typename Functor>
	bool for_each_block_in_reverse(Functor& functor) const
	{
		for (block_t* block = UT_LIST_GET_LAST(m_list);
		     block != NULL;
		     block = UT_LIST_GET_PREV(m_node, block)) {

			if (!functor(block)) {
				return(false);
			}
		}

		return(true);
	}

	/**
	@return the first block */
	block_t* front()
		MY_ATTRIBUTE((warn_unused_result))
	{
		ut_ad(UT_LIST_GET_LEN(m_list) > 0);
		return(UT_LIST_GET_FIRST(m_list));
	}

	/**
	@return true if m_first_block block was not filled fully */
	bool is_small() const
		MY_ATTRIBUTE((warn_unused_result))
	{
		return(m_heap == NULL);
	}

private:
	// Disable copying
	ppl_dyn_buf_t(const ppl_dyn_buf_t&);
	ppl_dyn_buf_t& operator=(const ppl_dyn_buf_t&);

	/**
	Add the block to the end of the list*/
	void push_back(block_t* block)
	{
		block->init();

		UT_LIST_ADD_LAST(m_list, block);
	}

	/** @return the last block in the list */
	block_t* back()
	{
		return(UT_LIST_GET_LAST(m_list));
	}

	/*
	@return true if request can be fullfilled */
	bool has_space(ulint size) const
	{
		return(back()->m_used + size <= MAX_DATA_SIZE);
	}

	/*
	@return true if request can be fullfilled */
	bool has_space(ulint size)
	{
		return(back()->m_used + size <= MAX_DATA_SIZE);
	}

	/** Find the block that contains the pos.
	@param pos	absolute offset, it is updated to make it relative
			to the block
	@return the block containing the pos. */
	block_t* find(ulint& pos)
	{
		block_t*	block;

		ut_ad(UT_LIST_GET_LEN(m_list) > 0);

		for (block = UT_LIST_GET_FIRST(m_list);
		     block != NULL;
		     block = UT_LIST_GET_NEXT(m_node, block)) {

			if (pos < block->used()) {
				break;
			}

			pos -= block->used();
		}

		ut_ad(block != NULL);
		ut_ad(block->used() >= pos);

		return(block);
	}

	/**
	Allocate and add a new block to m_list */
	block_t* add_block()
	{
		block_t*	block;

		if (m_heap == NULL) {
			m_heap = mem_heap_create(sizeof(*block));
		}

		block = reinterpret_cast<block_t*>(
			mem_heap_alloc(m_heap, sizeof(*block)));

		push_back(block);
		return(block);
	}

private:
	/** Heap to use for memory allocation */
	mem_heap_t*		m_heap;

	/** Allocated blocks */
	block_list_t		m_list;

	/** Total size used by all blocks */
	ulint			m_size;

	/** The default block, should always be the first element. This
	is for backwards compatibility and to avoid an extra heap allocation
	for small REDO log records */
	block_t			m_first_block;
};

typedef ppl_dyn_buf_t<PPL_DYN_ARRAY_DATA_SIZE> ppl_mtr_buf_t;
#endif


#endif /* dyn0buf_h */