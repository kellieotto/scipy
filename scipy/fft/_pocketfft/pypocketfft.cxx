/*
 * This file is part of pocketfft.
 * Licensed under a 3-clause BSD style license - see LICENSE.md
 */

/*
 *  Python interface.
 *
 *  Copyright (C) 2019 Max-Planck-Society
 *  \author Martin Reinecke
 */

#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include "pocketfft_hdronly.h"

namespace {

using namespace std;
using pocketfft::shape_t;
using pocketfft::stride_t;

namespace py = pybind11;

// Only instantiate long double transforms if they offer more precision
using ldbl_t = typename std::conditional<
  sizeof(long double)==sizeof(double), double, long double>::type;

auto c64 = py::dtype("complex64");
auto c128 = py::dtype("complex128");
auto clong = py::dtype("longcomplex");
auto f32 = py::dtype("float32");
auto f64 = py::dtype("float64");
auto flong = py::dtype("longfloat");
auto None = py::none();

shape_t copy_shape(const py::array &arr)
  {
  shape_t res(size_t(arr.ndim()));
  for (size_t i=0; i<res.size(); ++i)
    res[i] = size_t(arr.shape(int(i)));
  return res;
  }

stride_t copy_strides(const py::array &arr)
  {
  stride_t res(size_t(arr.ndim()));
  for (size_t i=0; i<res.size(); ++i)
    res[i] = arr.strides(int(i));
  return res;
  }

shape_t makeaxes(const py::array &in, const py::object &axes)
  {
  if (axes.is(None))
    {
    shape_t res(size_t(in.ndim()));
    for (size_t i=0; i<res.size(); ++i)
      res[i]=i;
    return res;
    }
  auto tmp=axes.cast<std::vector<ptrdiff_t>>();
  auto ndim = in.ndim();
  if ((tmp.size()>size_t(ndim)) || (tmp.size()==0))
    throw runtime_error("bad axes argument");
  for (auto& sz: tmp)
    {
    if (sz<0)
      sz += ndim;
    if ((sz>=ndim) || (sz<0))
      throw invalid_argument("axes exceeds dimensionality of output");
    }
  return shape_t(tmp.begin(), tmp.end());
  }

#define DISPATCH(arr, T1, T2, T3, func, args) \
  { \
  auto dtype = arr.dtype(); \
  if (dtype.is(T1)) return func<double> args; \
  if (dtype.is(T2)) return func<float> args; \
  if (dtype.is(T3)) return func<ldbl_t> args; \
  throw runtime_error("unsupported data type"); \
  }

template<typename T> T norm_fct(int inorm, size_t N)
  {
  if (inorm==0) return T(1);
  if (inorm==2) return T(1/ldbl_t(N));
  if (inorm==1) return T(1/sqrt(ldbl_t(N)));
  throw invalid_argument("invalid value for inorm (must be 0, 1, or 2)");
  }

template<typename T> T norm_fct(int inorm, const shape_t &shape,
  const shape_t &axes)
  {
  if (inorm==0) return T(1);
  size_t N(1);
  for (auto a: axes)
    N *= shape[a];
  return norm_fct<T>(inorm, N);
  }

template<typename T> py::array_t<T> prepare_output(py::object &out_,
  shape_t &dims)
  {
  if (out_.is(None)) return py::array_t<T>(dims);
  auto tmp = out_.cast<py::array_t<T>>();
  if (!tmp.is(out_)) // a new object was created during casting
    throw runtime_error("unexpected data type for output array");
  return tmp;
  }

template<typename T> py::array c2c_internal(const py::array &in,
  const py::object &axes_, bool forward, int inorm, py::object &out_,
  size_t nthreads)
  {
  auto axes = makeaxes(in, axes_);
  auto dims(copy_shape(in));
  auto res = prepare_output<complex<T>>(out_, dims);
  auto s_in=copy_strides(in);
  auto s_out=copy_strides(res);
  auto d_in=reinterpret_cast<const complex<T> *>(in.data());
  auto d_out=reinterpret_cast<complex<T> *>(res.mutable_data());
  {
  py::gil_scoped_release release;
  T fct = norm_fct<T>(inorm, dims, axes);
  pocketfft::c2c(dims, s_in, s_out, axes, forward, d_in, d_out, fct, nthreads);
  }
  return res;
  }

template<typename T> py::array c2c_sym_internal(const py::array &in,
  const py::object &axes_, bool forward, int inorm, py::object &out_,
  size_t nthreads)
  {
  auto axes = makeaxes(in, axes_);
  auto dims(copy_shape(in));
  auto res = prepare_output<complex<T>>(out_, dims);
  auto s_in=copy_strides(in);
  auto s_out=copy_strides(res);
  auto d_in=reinterpret_cast<const T *>(in.data());
  auto d_out=reinterpret_cast<complex<T> *>(res.mutable_data());
  {
  py::gil_scoped_release release;
  T fct = norm_fct<T>(inorm, dims, axes);
  pocketfft::r2c(dims, s_in, s_out, axes, forward, d_in, d_out, fct, nthreads);
  // now fill in second half
  using namespace pocketfft::detail;
  ndarr<complex<T>> ares(res.mutable_data(), dims, s_out);
  rev_iter iter(ares, axes);
  while(iter.remaining()>0)
    {
    auto v = ares[iter.ofs()];
    ares[iter.rev_ofs()] = conj(v);
    iter.advance();
    }
  }
  return res;
  }

py::array c2c(const py::array &a, const py::object &axes_, bool forward,
  int inorm, py::object &out_, size_t nthreads)
  {
  if (a.dtype().kind() == 'c')
    DISPATCH(a, c128, c64, clong, c2c_internal, (a, axes_, forward,
             inorm, out_, nthreads))

  DISPATCH(a, f64, f32, flong, c2c_sym_internal, (a, axes_, forward,
           inorm, out_, nthreads))
  }

template<typename T> py::array r2c_internal(const py::array &in,
  const py::object &axes_, bool forward, int inorm, py::object &out_,
  size_t nthreads)
  {
  auto axes = makeaxes(in, axes_);
  auto dims_in(copy_shape(in)), dims_out(dims_in);
  dims_out[axes.back()] = (dims_out[axes.back()]>>1)+1;
  py::array res = prepare_output<complex<T>>(out_, dims_out);
  auto s_in=copy_strides(in);
  auto s_out=copy_strides(res);
  auto d_in=reinterpret_cast<const T *>(in.data());
  auto d_out=reinterpret_cast<complex<T> *>(res.mutable_data());
  {
  py::gil_scoped_release release;
  T fct = norm_fct<T>(inorm, dims_in, axes);
  pocketfft::r2c(dims_in, s_in, s_out, axes, forward, d_in, d_out, fct,
    nthreads);
  }
  return res;
  }

py::array r2c(const py::array &in, const py::object &axes_, bool forward,
  int inorm, py::object &out_, size_t nthreads)
  {
  DISPATCH(in, f64, f32, flong, r2c_internal, (in, axes_, forward, inorm, out_,
    nthreads))
  }

template<typename T> py::array r2r_fftpack_internal(const py::array &in,
  const py::object &axes_, bool real2hermitian, bool forward, int inorm,
  py::object &out_, size_t nthreads)
  {
  auto axes = makeaxes(in, axes_);
  auto dims(copy_shape(in));
  py::array res = prepare_output<T>(out_, dims);
  auto s_in=copy_strides(in);
  auto s_out=copy_strides(res);
  auto d_in=reinterpret_cast<const T *>(in.data());
  auto d_out=reinterpret_cast<T *>(res.mutable_data());
  {
  py::gil_scoped_release release;
  T fct = norm_fct<T>(inorm, dims, axes);
  pocketfft::r2r_fftpack(dims, s_in, s_out, axes, real2hermitian, forward,
    d_in, d_out, fct, nthreads);
  }
  return res;
  }

py::array r2r_fftpack(const py::array &in, const py::object &axes_,
  bool real2hermitian, bool forward, int inorm, py::object &out_,
  size_t nthreads)
  {
  DISPATCH(in, f64, f32, flong, r2r_fftpack_internal, (in, axes_,
    real2hermitian, forward, inorm, out_, nthreads))
  }

template<typename T> py::array c2r_internal(const py::array &in,
  const py::object &axes_, size_t lastsize, bool forward, int inorm,
  py::object &out_, size_t nthreads)
  {
  auto axes = makeaxes(in, axes_);
  size_t axis = axes.back();
  shape_t dims_in(copy_shape(in)), dims_out=dims_in;
  if (lastsize==0) lastsize=2*dims_in[axis]-1;
  if ((lastsize/2) + 1 != dims_in[axis])
    throw runtime_error("bad lastsize");
  dims_out[axis] = lastsize;
  py::array res = prepare_output<T>(out_, dims_out);
  auto s_in=copy_strides(in);
  auto s_out=copy_strides(res);
  auto d_in=reinterpret_cast<const complex<T> *>(in.data());
  auto d_out=reinterpret_cast<T *>(res.mutable_data());
  {
  py::gil_scoped_release release;
  T fct = norm_fct<T>(inorm, dims_out, axes);
  pocketfft::c2r(dims_out, s_in, s_out, axes, forward, d_in, d_out, fct,
    nthreads);
  }
  return res;
  }

py::array c2r(const py::array &in, const py::object &axes_, size_t lastsize,
  bool forward, int inorm, py::object &out_, size_t nthreads)
  {
  DISPATCH(in, c128, c64, clong, c2r_internal, (in, axes_, lastsize, forward,
    inorm, out_, nthreads))
  }

template<typename T> py::array separable_hartley_internal(const py::array &in,
  const py::object &axes_, int inorm, py::object &out_, size_t nthreads)
  {
  auto dims(copy_shape(in));
  py::array res = prepare_output<T>(out_, dims);
  auto axes = makeaxes(in, axes_);
  auto s_in=copy_strides(in);
  auto s_out=copy_strides(res);
  auto d_in=reinterpret_cast<const T *>(in.data());
  auto d_out=reinterpret_cast<T *>(res.mutable_data());
  {
  py::gil_scoped_release release;
  T fct = norm_fct<T>(inorm, dims, axes);
  pocketfft::r2r_separable_hartley(dims, s_in, s_out, axes, d_in, d_out, fct,
    nthreads);
  }
  return res;
  }

py::array separable_hartley(const py::array &in, const py::object &axes_,
  int inorm, py::object &out_, size_t nthreads)
  {
  DISPATCH(in, f64, f32, flong, separable_hartley_internal, (in, axes_, inorm,
    out_, nthreads))
  }

template<typename T>py::array complex2hartley(const py::array &in,
  const py::array &tmp, const py::object &axes_, py::object &out_)
  {
  using namespace pocketfft::detail;
  auto dims_out(copy_shape(in));
  py::array out = prepare_output<T>(out_, dims_out);
  cndarr<cmplx<T>> atmp(tmp.data(), copy_shape(tmp), copy_strides(tmp));
  ndarr<T> aout(out.mutable_data(), copy_shape(out), copy_strides(out));
  auto axes = makeaxes(in, axes_);
  {
  py::gil_scoped_release release;
  simple_iter iin(atmp);
  rev_iter iout(aout, axes);
  if (iin.remaining()!=iout.remaining()) throw runtime_error("oops");
  while(iin.remaining()>0)
    {
    auto v = atmp[iin.ofs()];
    aout[iout.ofs()] = v.r+v.i;
    aout[iout.rev_ofs()] = v.r-v.i;
    iin.advance(); iout.advance();
    }
  }
  return out;
  }

py::array genuine_hartley(const py::array &in, const py::object &axes_,
  int inorm, py::object &out_, size_t nthreads)
  {
  auto tmp = r2c(in, axes_, true, inorm, None, nthreads);
  DISPATCH(in, f64, f32, flong, complex2hartley, (in, tmp, axes_, out_))
  }

const char *pypocketfft_DS = R"""(Fast Fourier and Hartley transforms.

This module supports
- single, double, and long double precision
- complex and real-valued transforms
- multi-dimensional transforms

For two- and higher-dimensional transforms the code will use SSE2 and AVX
vector instructions for faster execution if these are supported by the CPU and
were enabled during compilation.
)""";

const char *c2c_DS = R"""(Performs a complex FFT.

Parameters
----------
a : numpy.ndarray (any complex or real type)
    The input data. If its type is real, a more efficient real-to-complex
    transform will be used.
axes : list of integers
    The axes along which the FFT is carried out.
    If not set, all axes will be transformed.
forward : bool
    If `True`, a negative sign is used in the exponent, else a positive one.
inorm : int
    Normalization type
      0 : no normalization
      1 : divide by sqrt(N)
      2 : divide by N
    where N is the product of the lengths of the transformed axes.
out : numpy.ndarray (same shape as `a`, complex type with same accuracy as `a`)
    May be identical to `a`, but if it isn't, it must not overlap with `a`.
    If None, a new array is allocated to store the output.
nthreads : int
    Number of threads to use. If 0, use the system default (typically governed
    by the `OMP_NUM_THREADS` environment variable).

Returns
-------
numpy.ndarray (same shape as `a`, complex type with same accuracy as `a`)
    The transformed data.
)""";

const char *r2c_DS = R"""(Performs an FFT whose input is strictly real.

Parameters
----------
a : numpy.ndarray (any real type)
    The input data
axes : list of integers
    The axes along which the FFT is carried out.
    If not set, all axes will be transformed in ascending order.
forward : bool
    If `True`, a negative sign is used in the exponent, else a positive one.
inorm : int
    Normalization type
      0 : no normalization
      1 : divide by sqrt(N)
      2 : divide by N
    where N is the product of the lengths of the transformed input axes.
out : numpy.ndarray (complex type with same accuracy as `a`)
    For the required shape, see the `Returns` section.
    Must not overlap with `a`.
    If None, a new array is allocated to store the output.
nthreads : int
    Number of threads to use. If 0, use the system default (typically governed
    by the `OMP_NUM_THREADS` environment variable).

Returns
-------
numpy.ndarray (complex type with same accuracy as `a`)
    The transformed data. The shape is identical to that of the input array,
    except for the axis that was transformed last. If the length of that axis
    was n on input, it is n//2+1 on output.
)""";

const char *c2r_DS = R"""(Performs an FFT whose output is strictly real.

Parameters
----------
a : numpy.ndarray (any complex type)
    The input data
axes : list of integers
    The axes along which the FFT is carried out.
    If not set, all axes will be transformed in ascending order.
lastsize : the output size of the last axis to be transformed.
    If the corresponding input axis has size n, this can be 2*n-2 or 2*n-1.
forward : bool
    If `True`, a negative sign is used in the exponent, else a positive one.
inorm : int
    Normalization type
      0 : no normalization
      1 : divide by sqrt(N)
      2 : divide by N
    where N is the product of the lengths of the transformed output axes.
out : numpy.ndarray (real type with same accuracy as `a`)
    For the required shape, see the `Returns` section.
    Must not overlap with `a`.
    If None, a new array is allocated to store the output.
nthreads : int
    Number of threads to use. If 0, use the system default (typically governed
    by the `OMP_NUM_THREADS` environment variable).

Returns
-------
numpy.ndarray (real type with same accuracy as `a`)
    The transformed data. The shape is identical to that of the input array,
    except for the axis that was transformed last, which has now `lastsize`
    entries.
)""";

const char *r2r_fftpack_DS = R"""(Performs a real-valued FFT using the FFTPACK storage scheme.

Parameters
----------
a : numpy.ndarray (any real type)
    The input data
axes : list of integers
    The axes along which the FFT is carried out.
    If not set, all axes will be transformed.
real2hermitian : bool
    if True, the input is purely real and the output will have Hermitian
    symmetry and be stored in FFTPACK's halfcomplex ordering, otherwise the
    opposite.
forward : bool
    If `True`, a negative sign is used in the exponent, else a positive one.
inorm : int
    Normalization type
      0 : no normalization
      1 : divide by sqrt(N)
      2 : divide by N
    where N is the length of `axis`.
out : numpy.ndarray (same shape and data type as `a`)
    May be identical to `a`, but if it isn't, it must not overlap with `a`.
    If None, a new array is allocated to store the output.
nthreads : int
    Number of threads to use. If 0, use the system default (typically governed
    by the `OMP_NUM_THREADS` environment variable).

Returns
-------
numpy.ndarray (same shape and data type as `a`)
    The transformed data. The shape is identical to that of the input array.
)""";

const char *separable_hartley_DS = R"""(Performs a separable Hartley transform.
For every requested axis, a 1D forward Fourier transform is carried out, and
the real and imaginary parts of the result are added before the next axis is
processed.

Parameters
----------
a : numpy.ndarray (any real type)
    The input data
axes : list of integers
    The axes along which the transform is carried out.
    If not set, all axes will be transformed.
inorm : int
    Normalization type
      0 : no normalization
      1 : divide by sqrt(N)
      2 : divide by N
    where N is the product of the lengths of the transformed axes.
out : numpy.ndarray (same shape and data type as `a`)
    May be identical to `a`, but if it isn't, it must not overlap with `a`.
    If None, a new array is allocated to store the output.
nthreads : int
    Number of threads to use. If 0, use the system default (typically governed
    by the `OMP_NUM_THREADS` environment variable).

Returns
-------
numpy.ndarray (same shape and data type as `a`)
    The transformed data
)""";

const char *genuine_hartley_DS = R"""(Performs a full Hartley transform.
A full Fourier transform is carried out over the requested axes, and the
sum of real and imaginary parts of the result is stored in the output
array. For a single transformed axis, this is identical to `separable_hartley`,
but when transforming multiple axes, the results are different.

Parameters
----------
a : numpy.ndarray (any real type)
    The input data
axes : list of integers
    The axes along which the transform is carried out.
    If not set, all axes will be transformed.
inorm : int
    Normalization type
      0 : no normalization
      1 : divide by sqrt(N)
      2 : divide by N
    where N is the product of the lengths of the transformed axes.
out : numpy.ndarray (same shape and data type as `a`)
    May be identical to `a`, but if it isn't, it must not overlap with `a`.
    If None, a new array is allocated to store the output.
nthreads : int
    Number of threads to use. If 0, use the system default (typically governed
    by the `OMP_NUM_THREADS` environment variable).

Returns
-------
numpy.ndarray (same shape and data type as `a`)
    The transformed data
)""";

} // unnamed namespace

PYBIND11_MODULE(pypocketfft, m)
  {
  using namespace pybind11::literals;

  m.doc() = pypocketfft_DS;
  m.def("c2c", c2c, c2c_DS, "a"_a, "axes"_a=None, "forward"_a=true,
    "inorm"_a=0, "out"_a=None, "nthreads"_a=1);
  m.def("r2c", r2c, r2c_DS, "a"_a, "axes"_a=None, "forward"_a=true,
    "inorm"_a=0, "out"_a=None, "nthreads"_a=1);
  m.def("c2r", c2r, c2r_DS, "a"_a, "axes"_a=None, "lastsize"_a=0,
    "forward"_a=true, "inorm"_a=0, "out"_a=None, "nthreads"_a=1);
  m.def("r2r_fftpack", r2r_fftpack, r2r_fftpack_DS, "a"_a, "axes"_a,
    "real2hermitian"_a, "forward"_a, "inorm"_a=0, "out"_a=None, "nthreads"_a=1);
  m.def("separable_hartley", separable_hartley, separable_hartley_DS, "a"_a,
    "axes"_a=None, "inorm"_a=0, "out"_a=None, "nthreads"_a=1);
  m.def("genuine_hartley", genuine_hartley, genuine_hartley_DS, "a"_a,
    "axes"_a=None, "inorm"_a=0, "out"_a=None, "nthreads"_a=1);
  }
