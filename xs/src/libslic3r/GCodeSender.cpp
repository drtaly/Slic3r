#ifdef BOOST_LIBS
#include "GCodeSender.hpp"
#include <iostream>
#include <istream>
#include <string>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/lexical_cast.hpp>

#if __APPLE__
#include <sys/ioctl.h>
#include <termios.h>
#include <IOKit/serial/ioss.h>
#endif
#ifdef __linux
#include <sys/ioctl.h>
#include <termios.h>
#include <linux/serial.h>
#endif

namespace Slic3r {

namespace asio = boost::asio;

GCodeSender::GCodeSender()
    : io(), serial(io), can_send(false), sent(0), open(false), error(false),
      connected(false), queue_paused(false)
{}

GCodeSender::~GCodeSender()
{
    this->disconnect();
}

bool
GCodeSender::connect(std::string devname, unsigned int baud_rate)
{
    this->set_error_status(false);
    try {
        this->serial.open(devname);
    } catch (boost::system::system_error &e) {
        this->set_error_status(true);
        return false;
    }
    
    this->serial.set_option(asio::serial_port_base::parity(asio::serial_port_base::parity::odd));
    this->serial.set_option(asio::serial_port_base::character_size(asio::serial_port_base::character_size(8)));
    this->serial.set_option(asio::serial_port_base::flow_control(asio::serial_port_base::flow_control::none));
    this->serial.set_option(asio::serial_port_base::stop_bits(asio::serial_port_base::stop_bits::one));
    this->set_baud_rate(baud_rate);
    
    this->serial.close();
    this->serial.open(devname);
    this->serial.set_option(asio::serial_port_base::parity(asio::serial_port_base::parity::none));
    
    // set baud rate again because set_option overwrote it
    this->set_baud_rate(baud_rate);
    this->open = true;
    
    // this gives some work to the io_service before it is started
    // (post() runs the supplied function in its thread)
    this->io.post(boost::bind(&GCodeSender::do_read, this));
    
    // start reading in the background thread
    boost::thread t(boost::bind(&asio::io_service::run, &this->io));
    this->background_thread.swap(t);
    
    return true;
}

void
GCodeSender::set_baud_rate(unsigned int baud_rate)
{
    try {
        // This does not support speeds > 115200
        this->serial.set_option(asio::serial_port_base::baud_rate(baud_rate));
    } catch (boost::system::system_error &e) {
        boost::asio::serial_port::native_handle_type handle = this->serial.native_handle();

#if __APPLE__
        termios ios;
        ::tcgetattr(handle, &ios);
        ::cfsetspeed(&ios, baud_rate);
        speed_t newSpeed = baud_rate;
        ioctl(handle, IOSSIOSPEED, &newSpeed);
        ::tcsetattr(handle, TCSANOW, &ios);
#endif
#ifdef __linux
        termios ios;
        ::tcgetattr(handle, &ios);
        ::cfsetispeed(&ios, B38400);
        ::cfsetospeed(&ios, B38400);
        ::tcflush(handle, TCIFLUSH);
        ::tcsetattr(handle, TCSANOW, &ios);

        struct serial_struct ss;
        ioctl(handle, TIOCGSERIAL, &ss);
        ss.flags = (ss.flags & ~ASYNC_SPD_MASK) | ASYNC_SPD_CUST;
        ss.custom_divisor = (ss.baud_base + (baud_rate / 2)) / baud_rate;
        //cout << "bbase " << ss.baud_base << " div " << ss.custom_divisor;
        long closestSpeed = ss.baud_base / ss.custom_divisor;
        //cout << " Closest speed " << closestSpeed << endl;
        ss.reserved_char[0] = 0;
        if (closestSpeed < baud * 98 / 100 || closestSpeed > baud_rate * 102 / 100) {
            throw std::exception("Failed to set baud rate");
        }

        ioctl(handle, TIOCSSERIAL, &ss);
#else
        //throw invalid_argument ("OS does not currently support custom bauds");
#endif
    }
}

void
GCodeSender::disconnect()
{
    if (!this->open) return;

    this->open = false;
    this->connected = false;
    this->io.post(boost::bind(&GCodeSender::do_close, this));
    this->background_thread.join();
    this->io.reset();
    if (this->error_status()) {
        throw(boost::system::system_error(boost::system::error_code(),
            "Error while closing the device"));
    }
}

bool
GCodeSender::is_connected() const
{
    return this->connected;
}

bool
GCodeSender::wait_connected(unsigned int timeout) const
{
    using namespace boost::posix_time;
    ptime t0 = second_clock::local_time() + seconds(timeout);
    while (!this->connected) {
        if (second_clock::local_time() > t0) return false;
        boost::this_thread::sleep(boost::posix_time::milliseconds(100));
    }
    return true;
}

size_t
GCodeSender::queue_size() const
{
    boost::lock_guard<boost::mutex> l(this->queue_mutex);
    return this->queue.size();
}

void
GCodeSender::pause_queue()
{
    boost::lock_guard<boost::mutex> l(this->queue_mutex);
    this->queue_paused = true;
}

void
GCodeSender::resume_queue()
{
    {
        boost::lock_guard<boost::mutex> l(this->queue_mutex);
        this->queue_paused = false;
    }
    this->send();
}

void
GCodeSender::purge_queue(bool priority)
{
    boost::lock_guard<boost::mutex> l(this->queue_mutex);
    std::queue<std::string> empty;
    if (priority) {
        // clear priority queue
        std::swap(this->priqueue, empty);
    } else {
        // clear queue
        std::swap(this->queue, empty);
        this->queue_paused = false;
    }
}

// purge log and return its contents
std::vector<std::string>
GCodeSender::purge_log()
{
    boost::lock_guard<boost::mutex> l(this->log_mutex);
    std::vector<std::string> retval;
    retval.reserve(this->log.size());
    while (!this->log.empty()) {
        retval.push_back(this->log.front());
        this->log.pop();
    }
    return retval;
}

std::string
GCodeSender::getT() const
{
    boost::lock_guard<boost::mutex> l(this->log_mutex);
    return this->T;
}

std::string
GCodeSender::getB() const
{
    boost::lock_guard<boost::mutex> l(this->log_mutex);
    return this->B;
}

void
GCodeSender::do_close()
{
    this->set_error_status(false);
    boost::system::error_code ec;
    this->serial.cancel(ec);
    if (ec) this->set_error_status(true);
    this->serial.close(ec);
    if (ec) this->set_error_status(true);
}

void
GCodeSender::set_error_status(bool e)
{
    boost::lock_guard<boost::mutex> l(this->error_mutex);
    this->error = e;
}

bool
GCodeSender::error_status() const
{
    boost::lock_guard<boost::mutex> l(this->error_mutex);
    return this->error;
}

void
GCodeSender::do_read()
{
    // read one line
    asio::async_read_until(
        this->serial,
        this->read_buffer,
        '\n',
        boost::bind(
            &GCodeSender::on_read,
            this,
            asio::placeholders::error,
            asio::placeholders::bytes_transferred
        )
    );
}

void
GCodeSender::on_read(const boost::system::error_code& error,
    size_t bytes_transferred)
{
    this->set_error_status(false);
    if (error) {
        // error can be true even because the serial port was closed.
        // In this case it is not a real error, so ignore.
        if (this->open) {
            this->do_close();
            this->set_error_status(true);
        }
        return;
    }
    
    // copy the read buffer into string
    std::istream is(&this->read_buffer);
    std::string line;
    std::getline(is, line);
    // note that line might contain \r at its end
    
    // parse incoming line
    if (!this->connected
        && (boost::starts_with(line, "start")
         || boost::starts_with(line, "Grbl "))) {
        this->connected = true;
        {
            boost::lock_guard<boost::mutex> l(this->queue_mutex);
            this->can_send = true;
        }
        this->send();
    } else if (boost::starts_with(line, "ok")) {
        {
            boost::lock_guard<boost::mutex> l(this->queue_mutex);
            this->can_send = true;
        }
        this->send();
    } else if (boost::istarts_with(line, "resend")  // Marlin uses "Resend: "
            || boost::istarts_with(line, "rs")) {
        // extract the first number from line
        boost::algorithm::trim_left_if(line, !boost::algorithm::is_digit());
        size_t toresend = boost::lexical_cast<size_t>(line.substr(0, line.find_first_not_of("0123456789")));
        if (toresend == this->sent) {
            {
                boost::lock_guard<boost::mutex> l(this->queue_mutex);
                this->priqueue.push(this->last_sent);
                this->sent--;  // resend it with the same line number
                this->can_send = true;
            }
            this->send();
        } else {
            printf("Cannot resend %lu (last was %lu)\n", toresend, this->sent);
        }
    } else if (boost::starts_with(line, "wait")) {
        // ignore
    } else {
        // push any other line into the log
        boost::lock_guard<boost::mutex> l(this->log_mutex);
        this->log.push(line);
    }
    
    // parse temperature info
    {
        size_t pos = line.find("T:");
        if (pos != std::string::npos && line.size() > pos + 2) {
            // we got temperature info
            boost::lock_guard<boost::mutex> l(this->log_mutex);
            this->T = line.substr(pos+2, line.find_first_not_of("0123456789.", pos+2) - (pos+2));
        
            pos = line.find("B:");
            if (pos != std::string::npos && line.size() > pos + 2) {
                // we got bed temperature info
                this->B = line.substr(pos+2, line.find_first_not_of("0123456789.", pos+2) - (pos+2));
            }
        }
    }
    
    this->do_read();
}

void
GCodeSender::send(const std::vector<std::string> &lines, bool priority)
{
    // append lines to queue
    {
        boost::lock_guard<boost::mutex> l(this->queue_mutex);
        for (std::vector<std::string>::const_iterator line = lines.begin(); line != lines.end(); ++line) {
            if (priority) {
                this->priqueue.push(*line);
            } else {
                this->queue.push(*line);
            }
        }
    }
    this->send();
}

void
GCodeSender::send(const std::string &line, bool priority)
{
    // append line to queue
    {
        boost::lock_guard<boost::mutex> l(this->queue_mutex);
        if (priority) {
            this->priqueue.push(line);
        } else {
            this->queue.push(line);
        }
    }
    this->send();
}

void
GCodeSender::send()
{
    boost::lock_guard<boost::mutex> l(this->queue_mutex);
    
    // printer is not connected or we're still waiting for the previous ack
    if (!this->can_send) return;
    
    while (!this->priqueue.empty() || (!this->queue.empty() && !this->queue_paused)) {
        std::string line;
        if (!this->priqueue.empty()) {
            line = this->priqueue.front();
            this->priqueue.pop();
        } else {
            line = this->queue.front();
            this->queue.pop();
        }
        
        // strip comments
        if (size_t comment_pos = line.find_first_of(';') != std::string::npos)
        line.erase(comment_pos, std::string::npos);
        boost::algorithm::trim(line);
        
        // if line is not empty, send it
        if (!line.empty()) {
            this->do_send(line);
            return;
        }
        // if line is empty, process next item in queue
    }
}

// caller is responsible for holding queue_mutex
void
GCodeSender::do_send(const std::string &line)
{
    // compute full line
    this->sent++;
    std::string full_line = "N" + boost::lexical_cast<std::string>(this->sent) + " " + line;
    
    // calculate checksum
    int cs = 0;
    for (std::string::const_iterator it = full_line.begin(); it != full_line.end(); ++it)
       cs = cs ^ *it;
    
    // write line to device
    asio::streambuf b;
    std::ostream os(&b);
    os << full_line << "*" << cs << "\n";
    asio::write(this->serial, b);
    
    this->last_sent = line;
    this->can_send = false;
}

}

#ifdef SLIC3RXS
#include <myinit.h>
namespace Slic3r {
__REGISTER_CLASS(GCodeSender, "GCode::Sender");
}
#endif

#endif