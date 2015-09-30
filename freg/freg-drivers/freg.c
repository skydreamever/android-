#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/device.h>
#include <asm/uaccess.h>


#include "freg.h"

static int freg_major = 0;
static int freg_minor = 0;


static struct class *freg_class = NULL;
static struct fake_reg_dev *freg_dev = NULL;

static int freg_open(struct inode *inode,struct file *flip);
static int freg_release(struct inode *inode,struct file *flip);
static ssize_t freg_read(struct file *filp,char __user *buf,size_t count, loff_t *f_pos);
static ssize_t freg_write(struct file *filp,const char __user *buf,size_t count, loff_t *f_pos);

/*	这是一个传统文件系统的操作方法
第一个 file_operations 成员根本不是一个操作; 
它是一个指向拥有这个结构的模块的指针. 这个成员用来在它的操作还在被使用时阻止模块被卸载. 
几乎所有时间中, 它被简单初始化为 THIS_MODULE, 一个在 <linux/module.h> 中定义的宏.
*/
static struct file_operations freg_fops =
{//实验表明可以作为一种赋值方式，但是freg_fops不能是指针类型
	.owner = THIS_MODULE,
	.open = freg_open,
	.release = freg_release,
	.read = freg_read,
	.write = freg_write,
};

//这是devfs文件系统的操作方法
static ssize_t freg_val_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t freg_val_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);

//可以看出这个定义起来就简单多了，不用open和releas了
static DEVICE_ATTR(val, S_IRUGO | S_IWUSR, freg_val_show, freg_val_store);

//下面是传统设备的实现
static int freg_open(struct inode *inode,struct file *flip){
	struct fake_reg_dev *dev;
	//第一个参数为指向结构体中的成员的指针，第二个参数为结构体类型，第三个参数为结构体成员的名字，
	//那啥，下面这个可能有点绕，第一个dev是值得fake_reg_dev *dev，第二个是freg.h中定义的struct cdev dev;
	dev = container_of(inode->i_cdev,struct fake_reg_dev,dev);

	//实质就是把device设备的private_data指针指向了自己定义的结构体。增加可复用性。在读取的时候可以取到
	flip->private_data = dev;

	return 0;
}

static int freg_release(struct inode *inode,struct file *flip){
	return 0;
}

static ssize_t freg_read(struct file *flip,char __user *buf,size_t count, loff_t *f_pos){
	ssize_t err = 0;
	struct fake_reg_dev* dev = flip->private_data;

	if (down_interruptible(&(dev->sem))){
		return -ERESTARTSYS;
	}

	if (count < sizeof(dev->val)){
		goto out;
	}

	if (copy_to_user(buf,&(dev->val),sizeof(dev->val))){
		err = -EFAULT;
		goto out;
	}

	err = sizeof(dev->val);

out:
	up(&(dev->sem));
	return err;
}

static ssize_t freg_write(struct file *flip,const char __user *buf,size_t count, loff_t *f_pos){
	struct fake_reg_dev *dev = flip->private_data;
	ssize_t err = 0;

	if (down_interruptible(&(dev->sem))){
		return -ERESTARTSYS;
	}

	if(count != sizeof(dev->val)){
		goto out;
	}

	//看来不管是copy_to_user还是copy_from_user都是讲后边的拷贝到前面
	//而且成功返回0，注意，在很多的linux函数中，成功都返回0
	if(copy_from_user(&(dev->val),buf,count)){
		err = -EFAULT;
		goto out;
	}

	err = sizeof(dev->val);

out:
	up(&(dev->sem));
	return err;
}
//******************************************************************************************************************

//然后是devfs的操作方式
static ssize_t __freg_get_val(struct fake_reg_dev *dev, char *buf){
	int val = 0;

	if(down_interruptible(&(dev->sem))){
		return -ERESTARTSYS;
	}

	val = dev->val;
	up(&(dev->sem));

	return snprintf(buf,PAGE_SIZE,"%d\n",val);

}

static ssize_t __freg_set_val(struct fake_reg_dev *dev,const char *buf, size_t count){
	int val = 0;

	val = simple_strtol(buf, NULL, 10); //字符转换为int

	if(down_interruptible(&(dev->sem))){
		return -ERESTARTSYS;
	}
	dev->val = val;
	up(&(dev->sem));

	return count;
}


static ssize_t freg_val_show(struct device *dev, struct device_attribute *attr, char *buf){
	struct fake_reg_dev *hdev = (struct fake_reg_dev *)dev_get_drvdata(dev);//这个就是获得dev的成员函数driver_data，这个会在后面设置的

	return __freg_get_val(hdev,buf);
}

static ssize_t freg_val_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count){
	struct fake_reg_dev *hdev = (struct fake_reg_dev *)dev_get_drvdata(dev);//这个就是获得dev的成员函数driver_data，这个会在后面设置的

	return __freg_set_val(hdev,buf,count);

}



//******************************************************************************************************************

//下面是对文件系统的操作方法，定义文件为/proc/freg，直接对该类的变量freg_dev进行操作


static ssize_t freg_proc_read(char *page, char **start, off_t off, int count, int *eof, void *data){
	if (off > 0){
		*eof = 1;
		return 0;
	}

	return __freg_get_val(freg_dev,page);
}

static ssize_t freg_proc_write(struct file* flip, const char __user *buff, unsigned long len, void *data){

	int err = 0;
	char *page = NULL;

	if (len > PAGE_SIZE){
		printk(KERN_ALERT"The buff is to large: %lu.\n",len);
		return -EFAULT;
	}

	//下面是现将用户缓冲区的内容拷贝到内核缓冲区，然后再调用__freg_set_val
	//可以看出，只有在同一内核缓冲区的内容才能使用val = dev->val来复制，否则需要使用copy_from_user来不同缓冲区拷贝
	page = (char *)__get_free_page(GFP_KERNEL);//从内核中申请缓冲区，page现在是内核缓冲区，GFP_KERNEL表示无内存可用时可引起休眠。

	if (!page){
		printk(KERN_ALERT"Failed to alloc page.\n");
		return -ENOMEM;
	}

	if(copy_from_user(page,buff,len)){
		printk(KERN_ALERT"Failed to copy buff from user.\n");
		err = -EFAULT;
		goto out;
	}

	err = __freg_set_val(freg_dev,page,len);

out:
	free_page((unsigned long)page);
	return err;
}

static void freg_create_proc(void){
	struct proc_dir_entry *entry;

	//create_proc_entry这个函数可以接收一个文件名、一组权限和这个文件在 /proc 文件系统中出现的位置
	entry = create_proc_entry(FREG_DEVICE_PROC_NAME, 0, NULL);
	if (entry){
		entry->owner = THIS_MODULE;
		entry->read_proc = freg_proc_read;
		entry->write_proc = freg_proc_write;
	}
}

static void freg_remove_proc(void){
	remove_proc_entry(FREG_DEVICE_PROC_NAME,NULL);
}



//******************************************************************************************************************

//接下来就对设备初始化操作

//初始化设备
static int __freg_setup_dev(struct fake_reg_dev *dev){
	int err;
	dev_t devno = MKDEV(freg_major,freg_minor); //将主设备号和次设备号转换成dev_t类型的设备号

	memset(dev, 0, sizeof(struct fake_reg_dev));

	cdev_init(&(dev->dev),&freg_fops);//字符设备的传统操作方式
	dev->dev.owner = THIS_MODULE;

	err = cdev_add(&(dev->dev),devno,1);//将设备注册到设备号上面
	if(err){
		return err;
	}

	init_MUTEX(&(dev->sem));//初始化信号量
	dev->val = 0;

	return 0;
}


//初始化模块
static int __init freg_init(void){

	int err = -1;
	dev_t dev = 0;
	struct device *temp = NULL;

	printk(KERN_ALERT"Initializing freg device. \n");

	//动态分配字符设备（这个实际上只是分配设备号，需要下面的__freg_setup_dev讲设备绑定到设备号上面），同时生成主设备和冲设备号，
	//第二个参数，要分配的设备编号范围的初始值(次设备号常设为0)，第三个参数连续编号范围. 
	err = alloc_chrdev_region(&dev, 0, 1, FREG_DEVICE_NODE_NAME);

	if (err < 0){
		printk(KERN_ALERT"Failed to alloc char dev region.\n");
		goto fail;
	}

	freg_major = MAJOR(dev);
	freg_minor = MINOR(dev);

	freg_dev = kmalloc(sizeof(struct fake_reg_dev),GFP_KERNEL);

	if(!freg_dev){
		err = -ENOMEM;
		printk(KERN_ALERT"Failed to alloc freg device.\n");
		goto unregister;
	}

	err = __freg_setup_dev(freg_dev);//这里绑定了传统的设备操作方式

	if (err){
		printk(KERN_ALERT"Failed to setup freg device: %d.\n",err);
		goto cleanup;
	}

	freg_class = class_create(THIS_MODULE, FREG_DEVICE_CLASS_NAME);//在/sys/class目录中创建设备类别目录freg
	if(IS_ERR(freg_class)){
		err = PTR_ERR(freg_class);
		printk(KERN_ALERT"Failed to create freg device class. \n");
		goto destroy_cdev;
	}

	//在/dev目录和/sys/class/freg目录中分贝创建设备文件freg
	temp = device_create(freg_class,  NULL, dev, NULL, "%s", FREG_DEVICE_FILE_NAME);
	if (IS_ERR(temp)){
		err = PTR_ERR(temp);
		printk(KERN_ALERT"Failed to create freg device class. \n");
		goto destroy_class; 
	}

	//在/sys/class/freg/freg下面创建属性文件val
	err = device_create_file(temp, &dev_attr_val);//就是前面的DEVICE_ATTR，这里绑定了freg寄存器的val操作方式

	if (err < 0){
		printk(KERN_ALERT"Failed to create attribute val of freg device. \n");
		goto destroy_device;
	}

	dev_set_drvdata(temp, freg_dev);


	freg_create_proc();//这个是通过proc的方式进行操作

	printk(KERN_ALERT"Successded to initialize freg device");

	return 0;

destroy_device:
	device_destroy(freg_class, dev);
destroy_class:
	class_destroy(freg_class);
destroy_cdev:
	cdev_del(&(freg_dev->dev));	
cleanup:
	kfree(freg_dev);
unregister:
	unregister_chrdev_region(MKDEV(freg_major, freg_minor), 1);	
fail:
	return err;
}


static void __exit freg_exit(void) {
	dev_t devno = MKDEV(freg_major, freg_minor);

	printk(KERN_ALERT"Destroy freg device.\n");
	
	freg_remove_proc();

	if(freg_class) {
		device_destroy(freg_class, MKDEV(freg_major, freg_minor));
		class_destroy(freg_class);
	}

	if(freg_dev) {
		cdev_del(&(freg_dev->dev));
		kfree(freg_dev);
	}

	unregister_chrdev_region(devno, 1);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fake Register Driver");

module_init(freg_init);
module_exit(freg_exit);




